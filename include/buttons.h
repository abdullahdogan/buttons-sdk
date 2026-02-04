#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>  // for FILE*

#ifndef BUTTONS_MAX_LINES
#define BUTTONS_MAX_LINES 64
#endif

// Build/version helpers
const char *buttons_version(void);
void buttons_log_buildinfo(FILE *out);
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_EVENT_PRESS   = 1,  // fiziksel basýþ
    BTN_EVENT_RELEASE = 2,  // fiziksel býrakma
    BTN_EVENT_CLICK   = 3,  // kýsa basýþ (hold altý)
    BTN_EVENT_HOLD    = 4,  // uzun basma eþiði aþýldý
    BTN_EVENT_REPEAT  = 5   // hold sonrasý tekrar
} btn_event_t;

typedef struct {
    unsigned gpio;        // BCM GPIO
    bool     active_low;  // genelde true (pull-up)
    bool     enable_pull; // dahili pull-up/down kullan
} btn_pin_t;

typedef struct {
    const btn_pin_t *pins;
    unsigned count;

    unsigned debounce_ms; // 8–20 ms önerilir
    unsigned hold_ms;     // uzun basma eþiði
    unsigned repeat_ms;   // HOLD sonrasý tekrar aralýðý (0=kapalý)

    void *user; // kullanýcý verisi
    void (*on_event)(void *user, btn_event_t evt, unsigned index, unsigned gpio);
} btns_config_t;

typedef struct btns_ctx btns_ctx_t;

btns_ctx_t* btns_create(const btns_config_t *cfg);
void        btns_destroy(btns_ctx_t *ctx);
bool        btns_is_pressed(btns_ctx_t *ctx, unsigned index);

#ifdef __cplusplus
}
#endif
#endif
