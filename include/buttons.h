#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_EVENT_PRESS   = 1,  // fiziksel bas��
    BTN_EVENT_RELEASE = 2,  // fiziksel b�rakma
    BTN_EVENT_CLICK   = 3,  // k�sa bas�� (hold alt�)
    BTN_EVENT_HOLD    = 4,  // uzun basma e�i�i a��ld�
    BTN_EVENT_REPEAT  = 5   // hold sonras� tekrar
} btn_event_t;

typedef struct {
    unsigned gpio;        // BCM GPIO
    bool     active_low;  // genelde true (pull-up)
    bool     enable_pull; // dahili pull-up/down kullan
} btn_pin_t;

typedef struct {
    const btn_pin_t *pins;
    unsigned count;

    unsigned debounce_ms; // 8�20 ms �nerilir
    unsigned hold_ms;     // uzun basma e�i�i
    unsigned repeat_ms;   // HOLD sonras� tekrar aral��� (0=kapal�)

    void *user; // kullan�c� verisi
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
