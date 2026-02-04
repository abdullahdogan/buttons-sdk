// src/gpio_gpiod.c
// libgpiod v2 backend for edge events (safe for Debian Trixie/RPi Bookworm)
// - Uses request API with edge buffer
// - No per-event free (avoids double-free/invalid pointer on v2)
// - Optional debounce via line settings
// - Active-low configurable
// - Minimal surface: open / poll one event / close
// NOTE: If your header declares different symbol names or signatures,
// adjust the three exported functions at the bottom accordingly.

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gpiod.h>   // libgpiod v2

#include "buttons.h" // keep include to match project build; unused types are OK

// ---------- helpers ----------

static inline uint64_t timespec_to_ns(const struct timespec *ts) {
    return (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
}

static inline struct timespec ns_to_timespec(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    return ts;
}

static inline uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return timespec_to_ns(&ts);
}

// ---------- backend context ----------

struct gpio_backend_ctx {
    struct gpiod_chip *chip;
    struct gpiod_line_settings *ls_in;
    struct gpiod_line_config *lcfg;
    struct gpiod_request_config *rcfg;
    struct gpiod_line_request *req;
    struct gpiod_edge_event_buffer *evbuf;

    unsigned *offsets;
    size_t n_offsets;

    bool active_low;
    unsigned debounce_ms;

    // map offset -> index [0..n_offsets)
    // built once at open
    unsigned max_offset;     // highest offset value seen
    int *offset_to_index;    // length = max_offset + 1, -1 for not used
};

// Single-instance for simple library usage; extend if multi-instances are needed.
static struct gpio_backend_ctx *G = NULL;

// ---------- open ----------

static int build_offset_index_map(struct gpio_backend_ctx *ctx) {
    size_t i;
    unsigned maxo = 0;
    for (i = 0; i < ctx->n_offsets; ++i) {
        if (ctx->offsets[i] > maxo) maxo = ctx->offsets[i];
    }
    ctx->max_offset = maxo;
    ctx->offset_to_index = (int *)malloc((size_t)(maxo + 1) * sizeof(int));
    if (!ctx->offset_to_index) return -ENOMEM;
    for (unsigned o = 0; o <= maxo; ++o) ctx->offset_to_index[o] = -1;
    for (i = 0; i < ctx->n_offsets; ++i) {
        ctx->offset_to_index[ctx->offsets[i]] = (int)i;
    }
    return 0;
}

/*
 * Exported:
 * int buttons_gpio_open(const char *chip_name,
 *                       const unsigned *offsets, size_t n_offsets,
 *                       bool active_low,
 *                       unsigned debounce_ms);
 *
 * Returns 0 on success, negative errno on error.
 */
int buttons_gpio_open(const char *chip_name,
                      const unsigned *offsets, size_t n_offsets,
                      bool active_low,
                      unsigned debounce_ms)
{
    int ret = 0;
    size_t i;

    if (G) {
        // already open
        return -EBUSY;
    }
    if (!chip_name || !offsets || n_offsets == 0) {
        return -EINVAL;
    }

    struct gpio_backend_ctx *ctx = (struct gpio_backend_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) return -ENOMEM;

    ctx->offsets = (unsigned *)malloc(n_offsets * sizeof(unsigned));
    if (!ctx->offsets) { free(ctx); return -ENOMEM; }
    memcpy(ctx->offsets, offsets, n_offsets * sizeof(unsigned));
    ctx->n_offsets = n_offsets;
    ctx->active_low = active_low;
    ctx->debounce_ms = debounce_ms;

    ret = build_offset_index_map(ctx);
    if (ret) { free(ctx->offsets); free(ctx); return ret; }

    // 1) open chip
    ctx->chip = gpiod_chip_open_by_name(chip_name);
    if (!ctx->chip) {
        ret = -errno ? -errno : -ENODEV;
        goto fail;
    }

    // 2) line settings
    ctx->ls_in = gpiod_line_settings_new();
    if (!ctx->ls_in) { ret = -ENOMEM; goto fail; }

    gpiod_line_settings_set_direction(ctx->ls_in, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_edge_detection(ctx->ls_in, GPIOD_LINE_EDGE_BOTH);
    gpiod_line_settings_set_active_low(ctx->ls_in, active_low ? true : false);
    // Optional bias can be configured here if needed:
    // gpiod_line_settings_set_bias(ctx->ls_in, GPIOD_LINE_BIAS_PULL_UP / PULL_DOWN / DISABLED);

    if (debounce_ms > 0) {
        struct timespec period = ns_to_timespec((uint64_t)debounce_ms * 1000000ull);
        // Available on libgpiod v2:
        gpiod_line_settings_set_debounce_period(ctx->ls_in, &period);
    }

    // 3) line config
    ctx->lcfg = gpiod_line_config_new();
    if (!ctx->lcfg) { ret = -ENOMEM; goto fail; }

    // Apply same settings to all offsets
    if (gpiod_line_config_add_line_settings(ctx->lcfg, ctx->offsets, ctx->n_offsets, ctx->ls_in) != 0) {
        ret = -errno ? -errno : -EINVAL;
        goto fail;
    }

    // 4) request config
    ctx->rcfg = gpiod_request_config_new();
    if (!ctx->rcfg) { ret = -ENOMEM; goto fail; }
    gpiod_request_config_set_consumer(ctx->rcfg, "buttons-sdk");

    // 5) request lines
    ctx->req = gpiod_chip_request_lines(ctx->chip, ctx->rcfg, ctx->lcfg);
    if (!ctx->req) {
        ret = -errno ? -errno : -EIO;
        goto fail;
    }

    // 6) event buffer (capacity 32)
    ctx->evbuf = gpiod_edge_event_buffer_new(32);
    if (!ctx->evbuf) {
        ret = -ENOMEM;
        goto fail;
    }

    G = ctx;
    return 0;

fail:
    if (ctx->evbuf) {
        // Safe to free buffer object itself (do not free individual events).
        gpiod_edge_event_buffer_free(ctx->evbuf);
    }
    if (ctx->req) {
        gpiod_line_request_release(ctx->req);
    }
    if (ctx->rcfg) gpiod_request_config_free(ctx->rcfg);
    if (ctx->lcfg) gpiod_line_config_free(ctx->lcfg);
    if (ctx->ls_in) gpiod_line_settings_free(ctx->ls_in);
    if (ctx->chip) gpiod_chip_close(ctx->chip);

    free(ctx->offset_to_index);
    free(ctx->offsets);
    free(ctx);
    return ret;
}

// ---------- poll one event ----------

/*
 * Exported:
 * int buttons_gpio_poll(int timeout_ms, unsigned *index, int *edge);
 *
 * Waits for a single edge. On success returns 1 and fills:
 *  - *index = index within the provided offsets array [0..n-1]
 *  - *edge  = +1 for rising, 0 for falling
 * Returns 0 on timeout, negative errno on error.
 */
int buttons_gpio_poll(int timeout_ms, unsigned *index, int *edge)
{
    if (!G || !G->req) return -EINVAL;

    uint64_t to_ns = (timeout_ms < 0) ? 0 : (uint64_t)timeout_ms * 1000000ull;

    // 1) wait
    int rv = gpiod_line_request_wait_edge_events(G->req,
                                                 (timeout_ms < 0) ? NULL : &ns_to_timespec(to_ns));
    if (rv < 0) {
        return -errno ? -errno : -EIO;
    }
    if (rv == 0) {
        // timeout
        return 0;
    }

    // 2) read (up to buffer capacity)
    int n = gpiod_line_request_read_edge_events(G->req, G->evbuf, 32);
    if (n < 0) {
        return -errno ? -errno : -EIO;
    }
    if (n == 0) {
        // spurious wake
        return 0;
    }

    // Handle only the first event; drop the rest intentionally here.
    const struct gpiod_edge_event *ev = gpiod_edge_event_buffer_get_event(G->evbuf, 0);
    if (!ev) {
        return -EIO;
    }

    unsigned offset = gpiod_edge_event_get_line_offset(ev);
    int idx = -1;
    if (offset <= G->max_offset) idx = G->offset_to_index[offset];
    if (idx < 0) {
        // Unknown offset; ignore gracefully
        return 0;
    }

    enum gpiod_edge_event_type t = gpiod_edge_event_get_event_type((struct gpiod_edge_event *)ev);
    int is_rising = (t == GPIOD_EDGE_EVENT_RISING_EDGE) ? 1 : 0;

    if (index) *index = (unsigned)idx;
    if (edge)  *edge  = is_rising;

    // Do NOT free individual events on libgpiod v2.
    // Buffer object will be freed at close.

    return 1;
}

// ---------- close ----------

/*
 * Exported:
 * void buttons_gpio_close(void);
 */
void buttons_gpio_close(void)
{
    if (!G) return;

    // Safe to free the buffer object itself (do not free individual events).
    if (G->evbuf) gpiod_edge_event_buffer_free(G->evbuf);

    if (G->req)   gpiod_line_request_release(G->req);
    if (G->rcfg)  gpiod_request_config_free(G->rcfg);
    if (G->lcfg)  gpiod_line_config_free(G->lcfg);
    if (G->ls_in) gpiod_line_settings_free(G->ls_in);
    if (G->chip)  gpiod_chip_close(G->chip);

    free(G->offset_to_index);
    free(G->offsets);
    free(G);
    G = NULL;
}

/* End of file */
