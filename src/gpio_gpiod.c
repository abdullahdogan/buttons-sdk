// SPDX-License-Identifier: MIT
// GPIO backend for libgpiod v2.x
// Notes:
// - Uses gpiod v2 API (gpiod_chip_open, *_debounce_period_us, edge event buffer)
// - Never free single edge events (buffer owns them)
// - Builds a single request for all input lines with both-edge detection

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <poll.h>

#include <gpiod.h>
#include "buttons.h"    // keep existing public prototypes/types

#ifndef BUTTONS_MAX_LINES
#define BUTTONS_MAX_LINES 32
#endif

struct buttons_gpio_ctx {
    struct gpiod_chip            *chip;
    struct gpiod_line_settings   *ls_in;
    struct gpiod_line_config     *lc;
    struct gpiod_request_config  *rc;
    struct gpiod_line_request    *req;
    struct gpiod_edge_event_buffer *evbuf;

    unsigned offsets[BUTTONS_MAX_LINES];
    size_t   count;
    bool     active_low;
    uint32_t debounce_ms;
};

static int make_devpath(const char *chip_name, char out[128])
{
    if (!chip_name || !*chip_name)
        return -EINVAL;
    if (chip_name[0] == '/')
        snprintf(out, 128, "%s", chip_name);
    else
        snprintf(out, 128, "/dev/%s", chip_name);
    return 0;
}

int buttons_gpio_open(struct buttons_gpio_ctx **out,
                      const char *chip_name,
                      const unsigned *offsets,
                      size_t count,
                      bool active_low,
                      unsigned debounce_ms,
                      unsigned event_buf)
{
    if (!out || !chip_name || !offsets || !count || count > BUTTONS_MAX_LINES)
        return -EINVAL;

    *out = NULL;

    struct buttons_gpio_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -ENOMEM;

    ctx->count      = count;
    ctx->active_low = active_low;
    ctx->debounce_ms = debounce_ms ? debounce_ms : 0;

    for (size_t i = 0; i < count; i++)
        ctx->offsets[i] = offsets[i];

    char dev[128];
    int rc = make_devpath(chip_name, dev);
    if (rc) { free(ctx); return rc; }

    ctx->chip = gpiod_chip_open(dev);
    if (!ctx->chip) {
        rc = -errno ? -errno : -ENODEV;
        free(ctx);
        return rc;
    }

    // line settings: input, both edges, optional active-low and debounce
    ctx->ls_in = gpiod_line_settings_new();
    if (!ctx->ls_in) { rc = -ENOMEM; goto fail; }

    gpiod_line_settings_set_direction(ctx->ls_in, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_edge_detection(ctx->ls_in, GPIOD_LINE_EDGE_BOTH);

    if (ctx->active_low)
        gpiod_line_settings_set_active_low(ctx->ls_in, true);

    // Bias is left as "as is" unless buttons.h exposes a policy; keep default.

    if (ctx->debounce_ms > 0)
        gpiod_line_settings_set_debounce_period_us(ctx->ls_in,
                                                   (uint32_t)ctx->debounce_ms * 1000U);

    ctx->lc = gpiod_line_config_new();
    if (!ctx->lc) { rc = -ENOMEM; goto fail; }

    if (gpiod_line_config_add_line_settings(ctx->lc, ctx->offsets, (unsigned)ctx->count, ctx->ls_in)) {
        rc = -errno ? -errno : -EINVAL;
        goto fail;
    }

    ctx->rc = gpiod_request_config_new();
    if (!ctx->rc) { rc = -ENOMEM; goto fail; }

    gpiod_request_config_set_consumer(ctx->rc, "buttons-sdk");
    if (event_buf == 0) event_buf = 32;
    gpiod_request_config_set_event_buffer_size(ctx->rc, event_buf);

    ctx->req = gpiod_chip_request_lines(ctx->chip, ctx->rc, ctx->lc);
    if (!ctx->req) {
        rc = -errno ? -errno : -EIO;
        goto fail;
    }

    ctx->evbuf = gpiod_edge_event_buffer_new(event_buf);
    if (!ctx->evbuf) {
        rc = -ENOMEM;
        goto fail;
    }

    *out = ctx;
    return 0;

fail:
    if (ctx->evbuf) gpiod_edge_event_buffer_free(ctx->evbuf);
    if (ctx->req)   gpiod_line_request_release(ctx->req);
    if (ctx->rc)    gpiod_request_config_free(ctx->rc);
    if (ctx->lc)    gpiod_line_config_free(ctx->lc);
    if (ctx->ls_in) gpiod_line_settings_free(ctx->ls_in);
    if (ctx->chip)  gpiod_chip_close(ctx->chip);
    free(ctx);
    return rc;
}

void buttons_gpio_close(struct buttons_gpio_ctx *ctx)
{
    if (!ctx) return;
    if (ctx->evbuf) gpiod_edge_event_buffer_free(ctx->evbuf);
    if (ctx->req)   gpiod_line_request_release(ctx->req);
    if (ctx->rc)    gpiod_request_config_free(ctx->rc);
    if (ctx->lc)    gpiod_line_config_free(ctx->lc);
    if (ctx->ls_in) gpiod_line_settings_free(ctx->ls_in);
    if (ctx->chip)  gpiod_chip_close(ctx->chip);
    free(ctx);
}

// Helper: poll request FD
static int wait_fd(struct buttons_gpio_ctx *ctx, int timeout_ms)
{
    int fd = gpiod_line_request_get_poll_fd(ctx->req);
    if (fd < 0) return -EIO;

    struct pollfd p = { .fd = fd, .events = POLLIN };
    int pr = poll(&p, 1, timeout_ms);
    if (pr < 0) return -errno;
    if (pr == 0) return 0; // timeout
    if (p.revents & POLLIN) return 1;
    return -EIO;
}

// Public: poll for events and hand them to upper layer
// Returns:
//  >0 : number of events read
//   0 : timeout
//  <0 : negative errno-like
int buttons_gpio_poll(struct buttons_gpio_ctx *ctx, int timeout_ms,
                      int (*on_event)(unsigned offset, bool rising, uint64_t ts_ns, void *user),
                      void *user)
{
    if (!ctx || !ctx->req || !ctx->evbuf || !on_event) return -EINVAL;

    int w = wait_fd(ctx, timeout_ms);
    if (w <= 0) return w; // timeout or error

    int n = gpiod_line_request_read_edge_events(ctx->req, ctx->evbuf, (int)gpiod_request_config_get_event_buffer_size(ctx->rc));
    if (n < 0) return -errno ? -errno : -EIO;
    if (n == 0) return 0;

    for (int i = 0; i < n; i++) {
        const struct gpiod_edge_event *ev = gpiod_edge_event_buffer_get_event(ctx->evbuf, i);
        // v2 returns const event owned by buffer; do not free ev.
        bool rising = (gpiod_edge_event_get_event_type((struct gpiod_edge_event *)ev) == GPIOD_EDGE_EVENT_RISING_EDGE);
        unsigned off = gpiod_edge_event_get_line_offset((struct gpiod_edge_event *)ev);

        struct timespec ts = gpiod_edge_event_get_timestamp((struct gpiod_edge_event *)ev);
        uint64_t ts_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

        int rc = on_event(off, rising, ts_ns, user);
        if (rc) return rc; // allow upper layer to stop early
    }
    return n;
}

// Optional: read level helper (not all upper layers need this)
int buttons_gpio_read_level(struct buttons_gpio_ctx *ctx, unsigned offset, int *level_out)
{
    if (!ctx || !ctx->req || !level_out) return -EINVAL;
    // gpiod v2 allows snapshot reads via line_request_get_values, but we keep a single offset read
    int value;
    if (gpiod_line_request_get_value(ctx->req, offset, &value)) return -errno ? -errno : -EIO;
    *level_out = value;
    return 0;
}
