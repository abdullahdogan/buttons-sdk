// SPDX-License-Identifier: MIT
// GPIO backend for libgpiod v2.x
// Notes:
// - Uses gpiod v2 API (gpiod_chip_open, *_debounce_period_us, wait/read edge events)
// - Do not free single edge events (owned by buffer)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <gpiod.h>
#include "buttons.h"

#ifndef BUTTONS_MAX_LINES
#define BUTTONS_MAX_LINES 32
#endif

struct buttons_gpio_ctx {
    struct gpiod_chip              *chip;
    struct gpiod_line_settings     *ls_in;
    struct gpiod_line_config       *lc;
    struct gpiod_request_config    *rc;
    struct gpiod_line_request      *req;
    struct gpiod_edge_event_buffer *evbuf;

    unsigned offsets[BUTTONS_MAX_LINES];
    size_t   count;
    bool     active_low;
    uint32_t debounce_ms;
    unsigned buf_sz;
};

static int make_devpath(const char *chip_name, char out[128])
{
    if (!chip_name || !*chip_name) return -EINVAL;
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

    ctx->count       = count;
    ctx->active_low  = active_low;
    ctx->debounce_ms = debounce_ms ? debounce_ms : 0;
    ctx->buf_sz      = event_buf ? event_buf : 32;

    for (size_t i = 0; i < count; i++)
        ctx->offsets[i] = offsets[i];

    char dev[128];
    int rc = make_devpath(chip_name, dev);
    if (rc) { free(ctx); return rc; }

    ctx->chip = gpiod_chip_open(dev);
    if (!ctx->chip) { rc = -errno ? -errno : -ENODEV; goto fail_open; }

    ctx->ls_in = gpiod_line_settings_new();
    if (!ctx->ls_in) { rc = -ENOMEM; goto fail_open; }
    gpiod_line_settings_set_direction(ctx->ls_in, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_edge_detection(ctx->ls_in, GPIOD_LINE_EDGE_BOTH);
    if (ctx->active_low) gpiod_line_settings_set_active_low(ctx->ls_in, true);
    if (ctx->debounce_ms > 0)
        gpiod_line_settings_set_debounce_period_us(ctx->ls_in,
                                                   (uint32_t)ctx->debounce_ms * 1000U);

    ctx->lc = gpiod_line_config_new();
    if (!ctx->lc) { rc = -ENOMEM; goto fail_open; }
    if (gpiod_line_config_add_line_settings(ctx->lc, ctx->offsets, (unsigned)ctx->count, ctx->ls_in)) {
        rc = -errno ? -errno : -EINVAL; goto fail_open;
    }

    ctx->rc = gpiod_request_config_new();
    if (!ctx->rc) { rc = -ENOMEM; goto fail_open; }
    gpiod_request_config_set_consumer(ctx->rc, "buttons-sdk");
    gpiod_request_config_set_event_buffer_size(ctx->rc, ctx->buf_sz);

    ctx->req = gpiod_chip_request_lines(ctx->chip, ctx->rc, ctx->lc);
    if (!ctx->req) { rc = -errno ? -errno : -EIO; goto fail_open; }

    ctx->evbuf = gpiod_edge_event_buffer_new(ctx->buf_sz);
    if (!ctx->evbuf) { rc = -ENOMEM; goto fail_open; }

    *out = ctx;
    return 0;

fail_open:
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

static int wait_events(struct buttons_gpio_ctx *ctx, int timeout_ms)
{
    if (timeout_ms < 0) {
        int r = gpiod_line_request_wait_edge_events(ctx->req, NULL);
        if (r < 0) return -errno ? -errno : -EIO;
        return r; // 0 never returned with NULL timeout
    }
    struct timespec ts;
    ts.tv_sec  = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    int r = gpiod_line_request_wait_edge_events(ctx->req, &ts);
    if (r < 0) return -errno ? -errno : -EIO;
    return r; // 0 = timeout, >0 = ready
}

int buttons_gpio_poll(struct buttons_gpio_ctx *ctx, int timeout_ms,
                      int (*on_event)(unsigned offset, bool rising, uint64_t ts_ns, void *user),
                      void *user)
{
    if (!ctx || !ctx->req || !ctx->evbuf || !on_event) return -EINVAL;

    int w = wait_events(ctx, timeout_ms);
    if (w <= 0) return w; // timeout or error

    int n = gpiod_line_request_read_edge_events(ctx->req, ctx->evbuf, (int)ctx->buf_sz);
    if (n < 0) return -errno ? -errno : -EIO;
    if (n == 0) return 0;

    for (int i = 0; i < n; i++) {
        const struct gpiod_edge_event *cev = gpiod_edge_event_buffer_get_event(ctx->evbuf, i);
        bool rising = (gpiod_edge_event_get_event_type((struct gpiod_edge_event *)cev)
                        == GPIOD_EDGE_EVENT_RISING_EDGE);
        unsigned off = gpiod_edge_event_get_line_offset((struct gpiod_edge_event *)cev);
        uint64_t ts_ns = gpiod_edge_event_get_timestamp_ns((struct gpiod_edge_event *)cev);

        int rc = on_event(off, rising, ts_ns, user);
        if (rc) return rc;
    }
    return n;
}

int buttons_gpio_read_level(struct buttons_gpio_ctx *ctx, unsigned offset, int *level_out)
{
    if (!ctx || !ctx->req || !level_out) return -EINVAL;
    int value = gpiod_line_request_get_value(ctx->req, offset);
    if (value < 0) return -errno ? -errno : -EIO;
    *level_out = value;
    return 0;
}
