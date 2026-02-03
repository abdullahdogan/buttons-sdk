#include "buttons.h"
#include <linux/uinput.h>
#include <linux/input-event-codes.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <stdbool.h>

static int ufd = -1;

// --- PİN HARİTAN ---
// BCM numaraları: buton -> GND (aktif-low)
static const btn_pin_t PINS[] = {
    { .gpio=25, .active_low=true, .enable_pull=true }, // UP
    { .gpio=6, .active_low=true, .enable_pull=true }, // DOWN
    { .gpio=3, .active_low=true, .enable_pull=true }, // LEFT
    { .gpio=7,  .active_low=true, .enable_pull=true }, // RIGHT
    { .gpio=5, .active_low=true, .enable_pull=true }, // ENTER (örnek)
    { .gpio=27, .active_low=true, .enable_pull=true }, // ESC (örnek)
};

// Bu pinlere karşılık gelecek klavye tuşları
static const int KEYCODES[] = {
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_ENTER, KEY_ESC
};

// Davranış seçenekleri
static const bool ENABLE_SHIFT_ON_HOLD = true;  // uzun basışta SHIFT basılı tut
static const bool ENABLE_HOLD_MARKER   = false; // istersen HOLD başında F13 tap
static const int  HOLD_MARKER_KEY      = KEY_F13;

static bool shift_engaged[64];

static void emit(int type, int code, int val){
    struct input_event ie; memset(&ie, 0, sizeof ie);
    gettimeofday(&ie.time, NULL);
    ie.type = type; ie.code = code; ie.value = val;
    write(ufd, &ie, sizeof ie);
}
static void key_down(int key){ emit(EV_KEY, key, 1); emit(EV_SYN, SYN_REPORT, 0); }
static void key_up  (int key){ emit(EV_KEY, key, 0); emit(EV_SYN, SYN_REPORT, 0); }
static void key_tap (int key){ key_down(key); key_up(key); }

static volatile int run = 1;
static void sigint_handler(int s){ (void)s; run=0; }

static int uinput_open(void){
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_REP); // OS auto-repeat

    for (size_t i=0;i<sizeof(KEYCODES)/sizeof(KEYCODES[0]);++i)
        ioctl(fd, UI_SET_KEYBIT, KEYCODES[i]);
    if (ENABLE_SHIFT_ON_HOLD) ioctl(fd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
    if (ENABLE_HOLD_MARKER)   ioctl(fd, UI_SET_KEYBIT, HOLD_MARKER_KEY);

    struct uinput_setup us; memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_USB; us.id.vendor = 0x1; us.id.product = 0x1; us.id.version = 1;
    snprintf(us.name, UINPUT_MAX_NAME_SIZE, "Keypad HID (buttons-sdk)");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) { close(fd); return -1; }
    if (ioctl(fd, UI_DEV_CREATE, 0) < 0){ close(fd); return -1; }
    usleep(200*1000);
    return fd;
}

static void on_btn(void *user, btn_event_t evt, unsigned idx, unsigned gpio){
    (void)user; (void)gpio;
    if (idx >= (sizeof(KEYCODES)/sizeof(KEYCODES[0]))) return;
    int key = KEYCODES[idx];

    switch (evt){
    case BTN_EVENT_PRESS:
        key_down(key);
        break;
    case BTN_EVENT_HOLD:
        if (ENABLE_SHIFT_ON_HOLD && !shift_engaged[idx]) {
            key_down(KEY_LEFTSHIFT);
            shift_engaged[idx] = true;
        }
        if (ENABLE_HOLD_MARKER) key_tap(HOLD_MARKER_KEY);
        break;
    case BTN_EVENT_RELEASE:
        key_up(key);
        if (shift_engaged[idx]) { key_up(KEY_LEFTSHIFT); shift_engaged[idx]=false; }
        break;
    case BTN_EVENT_CLICK:
    case BTN_EVENT_REPEAT:
        // OS auto-repeat kullanılıyor; ekstra gerek yok
        break;
    }
}

int main(void){
    signal(SIGINT, sigint_handler);
    memset(shift_engaged, 0, sizeof(shift_engaged));

    // uinput modülü yüklü olmalı: sudo modprobe uinput (bir defa)
    ufd = uinput_open();
    if (ufd < 0){
        fprintf(stderr, "uinput açılamadı (/dev/uinput) — root ile çalıştırın.\n");
        return 1;
    }

    btns_config_t cfg = {
        .pins = PINS,
        .count = sizeof(PINS)/sizeof(PINS[0]),
        .debounce_ms = 12,
        .hold_ms = 600,  // uzun basış eşiği
        .repeat_ms = 0,  // OS auto-repeat
        .user = NULL,
        .on_event = on_btn
    };

    btns_ctx_t *ctx = btns_create(&cfg);
    if (!ctx){
        fprintf(stderr, "buttons init fail\n");
        ioctl(ufd, UI_DEV_DESTROY); close(ufd);
        return 1;
    }

    printf("keypad-hid: çalışıyor.\n");
    while (run) pause();

    btns_destroy(ctx);
    ioctl(ufd, UI_DEV_DESTROY); close(ufd);
    return 0;
}

