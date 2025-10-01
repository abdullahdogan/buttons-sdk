#include "buttons.h"
#include "gpio_backend.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    unsigned gpio;
    int  pull;            // 0/1/2
    bool active_low;
    bool pressed;         // debounced
    uint32_t last_edge_ms;
    uint32_t down_ms;
    bool hold_fired;
    uint32_t last_repeat_ms;
} btn_state_t;

struct btns_ctx {
    btns_config_t cfg;
    btn_state_t  *st;
    pthread_t     worker;
    volatile int  running;
};

static int find_index(struct btns_ctx *ctx, unsigned gpio){
    for (unsigned i=0;i<ctx->cfg.count;i++)
        if (ctx->st[i].gpio == gpio) return (int)i;
    return -1;
}

static void global_alert(int gpio, int level, uint32_t tick, void *userdata){
    (void)tick;
    struct btns_ctx *ctx = (struct btns_ctx*)userdata;
    if (!ctx) return;
    int idx = find_index(ctx, (unsigned)gpio);
    if (idx<0) return;

    btn_state_t *b = &ctx->st[idx];
    uint32_t t = gpio_now_ms();

    // Yazılımsal debounce (glitch filter zaten var)
    if ((t - b->last_edge_ms) < ctx->cfg.debounce_ms) return;
    b->last_edge_ms = t;

    bool logical_press = b->active_low ? (level==0) : (level==1);

    if (logical_press){
        b->pressed = true;
        b->down_ms = t;
        b->hold_fired = false;
        b->last_repeat_ms = t;
        if (ctx->cfg.on_event) ctx->cfg.on_event(ctx->cfg.user, BTN_EVENT_PRESS, (unsigned)idx, b->gpio);
    } else {
        bool was = b->pressed;
        b->pressed = false;
        if (was){
            if (ctx->cfg.on_event) ctx->cfg.on_event(ctx->cfg.user, BTN_EVENT_RELEASE,(unsigned)idx,b->gpio);
            uint32_t dur = t - b->down_ms;
            if (dur < ctx->cfg.hold_ms){
                if (ctx->cfg.on_event) ctx->cfg.on_event(ctx->cfg.user, BTN_EVENT_CLICK,(unsigned)idx,b->gpio);
            }
        }
    }
}

static void* worker(void *arg){
    struct btns_ctx *ctx = (struct btns_ctx*)arg;
    const unsigned poll = 10; // ms
    while (ctx->running){
        uint32_t t = gpio_now_ms();
        for (unsigned i=0;i<ctx->cfg.count;i++){
            btn_state_t *b = &ctx->st[i];
            if (b->pressed){
                uint32_t held = t - b->down_ms;
                if (!b->hold_fired && held >= ctx->cfg.hold_ms){
                    b->hold_fired = true;
                    if (ctx->cfg.on_event) ctx->cfg.on_event(ctx->cfg.user, BTN_EVENT_HOLD, i, b->gpio);
                    b->last_repeat_ms = t;
                }
                if (b->hold_fired && ctx->cfg.repeat_ms){
                    if ((t - b->last_repeat_ms) >= ctx->cfg.repeat_ms){
                        b->last_repeat_ms = t;
                        if (ctx->cfg.on_event) ctx->cfg.on_event(ctx->cfg.user, BTN_EVENT_REPEAT, i, b->gpio);
                    }
                }
            }
        }
        gpio_delay_ms(poll);
    }
    return NULL;
}

btns_ctx_t* btns_create(const btns_config_t *cfg){
    if (!cfg || !cfg->pins || cfg->count==0) return NULL;
    if (gpio_backend_init()!=0) return NULL;

    struct btns_ctx *ctx = calloc(1, sizeof(*ctx));
    ctx->cfg = *cfg;
    ctx->st  = calloc(cfg->count, sizeof(btn_state_t));

    for (unsigned i=0;i<cfg->count;i++){
        const btn_pin_t *p = &cfg->pins[i];
        btn_state_t *b = &ctx->st[i];
        b->gpio = p->gpio;
        b->active_low = p->active_low;
        b->pull = p->enable_pull ? (p->active_low ? 1 : 2) : 0;

        gpio_set_mode_input(p->gpio);
        gpio_set_pull(p->gpio, b->pull);

        unsigned us = (cfg->debounce_ms ? cfg->debounce_ms : 10) * 1000u;
        gpio_set_glitch_filter(p->gpio, us);

        // ÖNEMLİ: Backend sarmalayıcıyı kullan
        gpio_set_alert(p->gpio, global_alert, ctx);
    }

    ctx->running = 1;
    if (pthread_create(&ctx->worker, NULL, worker, ctx)!=0){
        free(ctx->st); free(ctx); gpio_backend_term(); return NULL;
    }
    return ctx;
}

void btns_destroy(btns_ctx_t *ctx){
    if (!ctx) return;
    ctx->running = 0;
    pthread_join(ctx->worker, NULL);

    for (unsigned i=0;i<ctx->cfg.count;i++){
        gpio_set_alert(ctx->cfg.pins[i].gpio, NULL, NULL);
        gpio_set_glitch_filter(ctx->cfg.pins[i].gpio, 0);
    }
    gpio_backend_term();
    free(ctx->st);
    free(ctx);
}

bool btns_is_pressed(btns_ctx_t *ctx, unsigned index){
    if (!ctx || index>=ctx->cfg.count) return false;
    return ctx->st[index].pressed;
}
