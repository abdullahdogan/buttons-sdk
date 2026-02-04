// SPDX-License-Identifier: MIT
// Keypad -> uinput virtual keyboard (libgpiod v2.x backend)
// Software repeat filter: drops events if same key repeats within min_gap_ms.
// All comments are ASCII only.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <errno.h>

#include "buttons.h"

#define ARRAYSZ(a) (sizeof(a)/sizeof(a[0]))

struct key_map {
    unsigned offset;
    int keycode;
};

struct app {
    struct buttons_gpio_ctx *gpio;
    struct key_map *map;
    size_t map_count;
    int ufd;
    unsigned min_gap_ms;

    // last send timestamp per key (ns)
    uint64_t last_sent_ns[256]; // simple small table for common keycodes
};

static int uinput_setup(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -errno;

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    // Allow common keys
    ioctl(fd, UI_SET_KEYBIT, KEY_ESC);
    ioctl(fd, UI_SET_KEYBIT, KEY_ENTER);
    ioctl(fd, UI_SET_KEYBIT, KEY_UP);
    ioctl(fd, UI_SET_KEYBIT, KEY_DOWN);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFT);
    ioctl(fd, UI_SET_KEYBIT, KEY_RIGHT);

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Keypad HID (buttons-sdk)");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x0001;
    uidev.id.product = 0x0001;
    uidev.id.version = 1;

    if (write(fd, &uidev, sizeof(uidev)) < 0) {
        int e = -errno;
        close(fd);
        return e;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        int e = -errno;
        close(fd);
        return e;
    }
    // small settle
    usleep(100 * 1000);
    return fd;
}

static void uinput_send(int fd, int keycode, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    clock_gettime(CLOCK_MONOTONIC, &ev.time);
    ev.type = EV_KEY;
    ev.code = keycode;
    ev.value = value;
    write(fd, &ev, sizeof(ev));

    memset(&ev, 0, sizeof(ev));
    clock_gettime(CLOCK_MONOTONIC, &ev.time);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(fd, &ev, sizeof(ev));
}

static int key_for_offset(struct app *app, unsigned off)
{
    for (size_t i = 0; i < app->map_count; i++)
        if (app->map[i].offset == off)
            return app->map[i].keycode;
    return -1;
}

static inline uint64_t ns_to_ms(uint64_t ns) { return ns / 1000000ULL; }

static int on_gpio_event(unsigned offset, bool rising, uint64_t ts_ns, void *user)
{
    struct app *app = (struct app *)user;
    int key = key_for_offset(app, offset);
    if (key < 0) return 0;

    // Simple edge -> press/release policy:
    // rising  => press(1)
    // falling => release(0)
    int value = rising ? 1 : 0;

    // Software min-gap filter (applied only to press events)
    if (value == 1) {
        uint64_t last = app->last_sent_ns[(key < 0 || key > 255) ? 0 : key];
        if (last) {
            uint64_t delta_ms = ns_to_ms(ts_ns - last);
            if (delta_ms < app->min_gap_ms)
                return 0; // drop rapid repeat
        }
        app->last_sent_ns[(key < 0 || key > 255) ? 0 : key] = ts_ns;
    }

    uinput_send(app->ufd, key, value);

    // Optional: generate short tap (press+release) for rising only
    // If hardware does not provide falling edges reliably, enable this:
    // if (rising) { usleep(5*1000); uinput_send(app->ufd, key, 0); }

    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --chip <gpiochipX or /dev/gpiochipX> [--active-low]\n"
        "           [--debounce-ms N] [--min-gap-ms N]\n"
        "           --map \"<off>:<name>,...\"\n"
        "Names: up,down,left,right,enter,esc\n",
        prog);
}

static int parse_map(const char *s, struct key_map *out, size_t *count)
{
    // Example: "17:up,22:down,23:left,24:right,25:enter,27:esc"
    char *tmp = strdup(s ? s : "");
    if (!tmp) return -ENOMEM;
    size_t n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (n >= BUTTONS_MAX_LINES) { free(tmp); return -EINVAL; }
        unsigned off = 0;
        char name[32] = {0};
        if (sscanf(tok, "%u:%31s", &off, name) != 2) { free(tmp); return -EINVAL; }
        int code = -1;
        if      (!strcmp(name, "up"))    code = KEY_UP;
        else if (!strcmp(name, "down"))  code = KEY_DOWN;
        else if (!strcmp(name, "left"))  code = KEY_LEFT;
        else if (!strcmp(name, "right")) code = KEY_RIGHT;
        else if (!strcmp(name, "enter")) code = KEY_ENTER;
        else if (!strcmp(name, "esc"))   code = KEY_ESC;
        else { free(tmp); return -EINVAL; }

        out[n].offset  = off;
        out[n].keycode = code;
        n++;
    }
    *count = n;
    free(tmp);
    return 0;
}

int main(int argc, char **argv)
{
    const char *chip = "gpiochip0";
    bool active_low = false;
    unsigned debounce_ms = 35;
    unsigned min_gap_ms = 150;
    const char *map_str = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chip") && i+1 < argc)          chip = argv[++i];
        else if (!strcmp(argv[i], "--active-low"))             active_low = true;
        else if (!strcmp(argv[i], "--debounce-ms") && i+1 < argc) debounce_ms = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--min-gap-ms") && i+1 < argc)  min_gap_ms = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--map") && i+1 < argc)      map_str = argv[++i];
        else { usage(argv[0]); return 2; }
    }

    if (!map_str) {
        fprintf(stderr, "Missing --map\n");
        usage(argv[0]);
        return 2;
    }

    struct key_map map[BUTTONS_MAX_LINES];
    size_t map_count = 0;
    if (parse_map(map_str, map, &map_count)) {
        fprintf(stderr, "Bad --map format\n");
        return 2;
    }

    unsigned offsets[BUTTONS_MAX_LINES];
    for (size_t i = 0; i < map_count; i++) offsets[i] = map[i].offset;

    struct app app;
    memset(&app, 0, sizeof(app));
    app.map = map;
    app.map_count = map_count;
    app.min_gap_ms = min_gap_ms;

    if (buttons_gpio_open(&app.gpio, chip, offsets, map_count, active_low, debounce_ms, 64)) {
        fprintf(stderr, "GPIO open failed: %s\n", strerror(errno));
        return 1;
    }

    app.ufd = uinput_setup();
    if (app.ufd < 0) {
        fprintf(stderr, "uinput setup failed: %s\n", strerror(-app.ufd));
        buttons_gpio_close(app.gpio);
        return 1;
    }

    // Main loop
    for (;;) {
        int r = buttons_gpio_poll(app.gpio, 1000, on_gpio_event, &app);
        if (r < 0) {
            // transient errors can be ignored or break depending on policy
            // here we just continue
            usleep(5 * 1000);
        }
    }

    // never reached
    // UI_DEV_DESTROY and close will be done by OS on exit
    return 0;
}
