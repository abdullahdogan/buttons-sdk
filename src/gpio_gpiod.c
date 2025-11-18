#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <gpiod.h>

#include "buttons.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#endif

/* CMake tarafı: USE_GPIOD_V2 veya USE_GPIOD_V1 tanımlar.
 * Biri tanımlı değilse, derlemeyi durdur.
 */
#if !defined(USE_GPIOD_V2) && !defined(USE_GPIOD_V1)
# error "Define USE_GPIOD_V2 or USE_GPIOD_V1 from build system."
#endif

typedef void (*btn_level_cb)(unsigned gpio, int level, void *user);

struct reg_item {
    unsigned        gpio;
    bool            active_low;
    bool            enable_pull;
    btn_level_cb    cb;
    void           *user;
#if defined(USE_GPIOD_V1)
    struct gpiod_line *line;
#elif defined(USE_GPIOD_V2)
    struct gpiod_line_request *req;
#endif
};

static pthread_t          g_thr;
static volatile int       g_run = 0;
static struct gpiod_chip *g_chip = NULL;

#define MAX_REGS 64
static struct reg_item g_regs[MAX_REGS];
static unsigned        g_reg_count = 0;

static int effective_level(int rising_edge, bool active_low)
{
    int level = rising_edge ? 1 : 0;
    return active_low ? !level : level;
}

#if defined(USE_GPIOD_V2)
/* ---- v2: tek pin için request oluştur ---- */
static struct gpiod_line_request* v2_request_one(struct gpiod_chip *chip,
                                                  unsigned offset,
                                                  bool enable_pull,
                                                  bool active_low)
{
    struct gpiod_line_settings *ls = gpiod_line_settings_new();
    if (!ls) return NULL;

    gpiod_line_settings_set_direction(ls, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_edge_detection(ls, GPIOD_LINE_EDGE_BOTH);

    if (enable_pull) {
        gpiod_line_settings_set_bias(ls,
            active_low ? GPIOD_LINE_BIAS_PULL_UP : GPIOD_LINE_BIAS_PULL_DOWN);
    } else {
        gpiod_line_settings_set_bias(ls, GPIOD_LINE_BIAS_AS_IS);
    }

    struct gpiod_line_config *lc = gpiod_line_config_new();
    if (!lc) { gpiod_line_settings_free(ls); return NULL; }
    if (gpiod_line_config_add_line_settings(lc, &offset, 1, ls)) {
        gpiod_line_settings_free(ls);
        gpiod_line_config_free(lc);
        return NULL;
    }
    gpiod_line_settings_free(ls);

    struct gpiod_request_config *rc = gpiod_request_config_new();
    if (!rc) { gpiod_line_config_free(lc); return NULL; }
    gpiod_request_config_set_consumer(rc, "buttons-sdk");
    gpiod_request_config_set_event_buffer_size(rc, 16);

    struct gpiod_line_request *req = gpiod_chip_request_lines(chip, rc, lc);
    gpiod_request_config_free(rc);
    gpiod_line_config_free(lc);
    return req;
}
#endif

/* ---- İzleme thread'i ---- */
static void* monitor_thread(void *arg)
{
    (void)arg;
    g_run = 1;

#if defined(USE_GPIOD_V2)
    struct pollfd pfds[MAX_REGS] = {0};
    for (;;) {
        if (!g_run) break;

        unsigned n = 0;
        for (unsigned i = 0; i < g_reg_count; i++) {
            if (!g_regs[i].req) continue;
            pfds[n].fd = gpiod_line_request_get_fd(g_regs[i].req);
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            n++;
        }
        if (n == 0) { usleep(1000*100); continue; }

        int rc = poll(pfds, n, 1000);
        if (rc <= 0) continue;

        unsigned idx = 0;
        for (unsigned i = 0; i < g_reg_count; i++) {
            if (!g_regs[i].req) { idx++; continue; }
            if (!(pfds[idx].revents & POLLIN)) { idx++; continue; }

            struct gpiod_edge_event_buffer *buf = gpiod_edge_event_buffer_new(16);
            if (!buf) { idx++; continue; }

            int nread = gpiod_line_request_read_edge_events(g_regs[i].req, buf, 16);
            for (int k = 0; k < nread; k++) {
                const struct gpiod_edge_event *ev = gpiod_edge_event_buffer_get_event(buf, k);
                if (!ev) continue;
                int rising = (gpiod_edge_event_get_event_type(ev) == GPIOD_EDGE_EVENT_RISING_EDGE);
                int level  = effective_level(rising, g_regs[i].active_low);
                if (g_regs[i].cb) g_regs[i].cb(g_regs[i].gpio, level, g_regs[i].user);
            }
            gpiod_edge_event_buffer_free(buf);
            idx++;
        }
    }
#elif defined(USE_GPIOD_V1)
    struct pollfd pfds[MAX_REGS] = {0};
    for (;;) {
        if (!g_run) break;

        unsigned n = 0;
        for (unsigned i = 0; i < g_reg_count; i++) {
            if (!g_regs[i].line) continue;
            pfds[n].fd = gpiod_line_event_get_fd(g_regs[i].line);
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            n++;
        }
        if (n == 0) { usleep(1000*100); continue; }

        int rc = poll(pfds, n, 1000);
        if (rc <= 0) continue;

        unsigned idx = 0;
        for (unsigned i = 0; i < g_reg_count; i++) {
            if (!g_regs[i].line) { idx++; continue; }
            if (!(pfds[idx].revents & POLLIN)) { idx++; continue; }

            struct gpiod_line_event ev;
            while (gpiod_line_event_read(g_regs[i].line, &ev) == 0) {
                int rising = (ev.event_type == GPIOD_LINE_EVENT_RISING_EDGE);
                int level  = effective_level(rising, g_regs[i].active_low);
                if (g_regs[i].cb) g_regs[i].cb(g_regs[i].gpio, level, g_regs[i].user);
            }
            idx++;
        }
    }
#endif
    return NULL;
}

/* ---- Backend API ---- */
int gpio_backend_init(void)
{
    if (g_chip) return 0;
    g_chip = gpiod_chip_open("/dev/gpiochip0");
    if (!g_chip) return -1;

    g_reg_count = 0;
    g_run = 1;
    if (pthread_create(&g_thr, NULL, monitor_thread, NULL) != 0) {
        gpiod_chip_close(g_chip); g_chip = NULL;
        g_run = 0;
        return -1;
    }
    return 0;
}

void gpio_backend_term(void)
{
    g_run = 0;
    if (g_thr) { pthread_join(g_thr, NULL); }

#if defined(USE_GPIOD_V1)
    for (unsigned i = 0; i < g_reg_count; i++) {
        if (g_regs[i].line) {
            gpiod_line_release(g_regs[i].line);
            g_regs[i].line = NULL;
        }
    }
#elif defined(USE_GPIOD_V2)
    for (unsigned i = 0; i < g_reg_count; i++) {
        if (g_regs[i].req) {
            gpiod_line_request_release(g_regs[i].req);
            g_regs[i].req = NULL;
        }
    }
#endif
    g_reg_count = 0;

    if (g_chip) { gpiod_chip_close(g_chip); g_chip = NULL; }
}

int gpio_set_alert(unsigned gpio, bool active_low, bool enable_pull,
                   void (*cb)(unsigned gpio, int level, void *user), void *user)
{
    if (!g_chip) return -1;
    if (g_reg_count >= MAX_REGS) return -1;

    struct reg_item item = {0};
    item.gpio        = gpio;
    item.active_low  = active_low;
    item.enable_pull = enable_pull;
    item.cb          = (btn_level_cb)cb;
    item.user        = user;

#if defined(USE_GPIOD_V2)
    item.req = v2_request_one(g_chip, gpio, enable_pull, active_low);
    if (!item.req) return -1;
#elif defined(USE_GPIOD_V1)
    struct gpiod_line *line = gpiod_chip_get_line(g_chip, gpio);
    if (!line) return -1;

    struct gpiod_line_request_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.consumer = "buttons-sdk";
    cfg.request_type = GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES;

    int flags = 0;
#ifdef GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP
    if (enable_pull && active_low)  flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP;
#endif
#ifdef GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN
    if (enable_pull && !active_low) flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN;
#endif
    cfg.flags = flags;

    if (gpiod_line_request(line, &cfg, 0) != 0) return -1;
    item.line = line;
#endif

    g_regs[g_reg_count++] = item;
    return 0;
}