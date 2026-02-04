// SPDX-License-Identifier: MIT
// Virtual keyboard daemon (GPIO -> uinput) with libgpiod v2 backend
// ASCII-only comments.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/uinput.h>
#include <linux/input.h>

#include "buttons.h"  // provides BUTTONS_MAX_LINES and gpio helpers

struct key_map {
    unsigned offset;   // GPIO line offset
    int keycode;       // Linux input key code
};

struct state_per_line {
    uint64_t last_ts_ns;  // last event timestamp (ns)
    int last_level;       // -1 unknown, 0 released, 1 pressed
};

struct app_ctx {
    int ufd;
    struct buttons_gpio_ctx *gpio;
    struct key_map *map;
    size_t map_count;
    unsigned min_gap_ms;
    struct state_per_line st[BUTTONS_MAX_LINES];
};

// --- time helpers ---

static uint64_t tv_to_ns(const struct timeval *tv)
{
    return (uint64_t)tv->tv_sec * 1000000000ull + (uint64_t)tv->tv_usec * 1000ull;
}

// --- uinput helpers ---

static int uinput_open(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -errno ? -errno : -ENODEV;
    return fd;
}

static int uinput_setup_keyboard(int fd, const int *keycodes, size_t n)
{
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) return -errno;
    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) return -errno;

    // Do not enable EV_REP to avoid OS auto-repeat from this device.

    for (size_t i = 0; i < n; i++) {
        if (keycodes[i] > 0)
            if (ioctl(fd, UI_SET_KEYBIT, keycodes[i]) < 0) return -errno;
    }

    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_USB;
    us.id.vendor  = 0x0001;
    us.id.product = 0x0001;
    us.id.version = 0x0001;
    snprintf(us.name, sizeof(us.name), "Keypad HID (buttons-sdk)");

    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) return -errno;
    if (ioctl(fd, UI_DEV_CREATE) < 0) return -errno;
    return 0;
}

static int uinput_emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL); // timeval expected by input_event
    ev.type  = type;
    ev.code  = code;
    ev.value = value;

    ssize_t w = write(fd, &ev, sizeof(ev));
    if (w != (ssize_t)sizeof(ev)) return -EIO;

    struct input_event syn;
    memset(&syn, 0, sizeof(syn));
    gettimeofday(&syn.time, NULL);
    syn.type = EV_SYN;
    syn.code = SYN_REPORT;
    syn.value = 0;
    w = write(fd, &syn, sizeof(syn));
    if (w != (ssize_t)sizeof(syn)) return -EIO;

    return 0;
}

static int uinput_send_key(int fd, int keycode, int value01)
{
    return uinput_emit(fd, EV_KEY, (uint16_t)keycode, value01);
}

// --- key map parse ---

static int keyname_to_code(const char *name)
{
    // Accept a small set; extend as needed.
    if (strcasecmp(name, "up")    == 0) return KEY_UP;
    if (strcasecmp(name, "down")  == 0) return KEY_DOWN;
    if (strcasecmp(name, "left")  == 0) return KEY_LEFT;
    if (strcasecmp(name, "right") == 0) return KEY_RIGHT;
    if (strcasecmp(name, "enter") == 0) return KEY_ENTER;
    if (strcasecmp(name, "esc")   == 0 || strcasecmp(name, "escape") == 0) return KEY_ESC;

    // Also allow numeric codes (e.g., 30=A).
    char *end = NULL;
    long v = strtol(name, &end, 0);
    if (end && *end == '\0' && v > 0 && v < 1024) return (int)v;

    return -EINVAL;
}

static int parse_map(const char *spec, struct key_map *out, size_t *count_out)
{
    // Format: "17:up,22:down,23:left,24:right,25:enter,27:esc"
    if (!spec || !out || !count_out) return -EINVAL;
    char *tmp = strdup(spec);
    if (!tmp) return -ENOMEM;

    size_t n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ",", &save);
         tok && n < BUTTONS_MAX_LINES;
         tok = strtok_r(NULL, ",", &save))
    {
        char *colon = strchr(tok, ':');
        if (!colon) { free(tmp); return -EINVAL; }
        *colon = '\0';
        const char *s_off = tok;
        const char *s_key = colon + 1;

        char *e1 = NULL;
        long off = strtol(s_off, &e1, 0);
        if (!e1 || *e1 != '\0' || off < 0 || off > 1023) { free(tmp); return -EINVAL; }

        int code = keyname_to_code(s_key);
        if (code < 0) { free(tmp); return -EINVAL; }

        out[n].offset  = (unsigned)off;
        out[n].keycode = code;
        n++;
    }

    free(tmp);
    *count_out = n;
    return (n > 0) ? 0 : -EINVAL;
}

static int build_offsets_array(const struct key_map *m, size_t n, unsigned *offs_out)
{
    for (size_t i = 0; i < n; i++) offs_out[i] = m[i].offset;
    return 0;
}

static int find_keycode_by_offset(const struct app_ctx *app, unsigned offset)
{
    for (size_t i = 0; i < app->map_count; i++)
        if (app->map[i].offset == offset) return app->map[i].keycode;
    return -1;
}

// --- event callback ---

static int on_gpio_event(unsigned offset, bool rising, uint64_t ts_ns, void *user)
{
    struct app_ctx *app = (struct app_ctx *)user;

    // Logical press on rising, release on falling (active-low handled in backend).
    int level = rising ? 1 : 0;

    // Per-line state slot index lookup
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < app->map_count; i++)
        if (app->map[i].offset == offset) { idx = i; break; }
    if (idx == SIZE_MAX) return 0; // unmapped, ignore

    struct state_per_line *st = &app->st[idx];

    // Gap filter: ignore if same-kind event within min_gap_ms
    uint64_t gap_ns = (uint64_t)app->min_gap_ms * 1000000ull;
    if (st->last_level == level) {
        if (st->last_ts_ns != 0 && ts_ns - st->last_ts_ns < gap_ns)
            return 0;
    }

    int keycode = app->map[idx].keycode;
    if (keycode < 0) return 0;

    // Avoid duplicate emits: only emit when level actually changes
    if (st->last_level != level) {
        int rc = uinput_send_key(app->ufd, keycode, level ? 1 : 0);
        if (rc) return rc;
        st->last_level = level;
        st->last_ts_ns = ts_ns;
    } else {
        // Same level but passed gap; update timestamp only
        st->last_ts_ns = ts_ns;
    }

    return 0;
}

// --- args and main ---

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--chip <name_or_path>] [--active-low] [--debounce-ms N]\n"
        "          [--min-gap-ms N] --map \"off:key,...\"\n"
        "Example: %s --chip gpiochip0 --active-low --debounce-ms 35 \\\n"
        "          --min-gap-ms 150 --map \"17:up,22:down,23:left,24:right,25:enter,27:esc\"\n",
        prog, prog);
}

int main(int argc, char **argv)
{
    const char *chip = "gpiochip0";
    bool active_low = false;
    unsigned debounce_ms = 35;
    unsigned min_gap_ms = 150;
    const char *map_spec = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chip") && i + 1 < argc) { chip = argv[++i]; continue; }
        if (!strcmp(argv[i], "--active-low")) { active_low = true; continue; }
        if (!strcmp(argv[i], "--debounce-ms") && i + 1 < argc) { debounce_ms = (unsigned)strtoul(argv[++i], NULL, 10); continue; }
        if (!strcmp(argv[i], "--min-gap-ms") && i + 1 < argc) { min_gap_ms = (unsigned)strtoul(argv[++i], NULL, 10); continue; }
        if (!strcmp(argv[i], "--map") && i + 1 < argc) { map_spec = argv[++i]; continue; }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        fprintf(stderr, "Unknown option: %s\n", argv[i]); usage(argv[0]); return 2;
    }

    if (!map_spec) {
        fprintf(stderr, "--map is required.\n"); usage(argv[0]); return 2;
    }

    struct key_map map[BUTTONS_MAX_LINES];
    size_t map_count = 0;
    if (parse_map(map_spec, map, &map_count) != 0) {
        fprintf(stderr, "Invalid --map.\n");
        return 2;
    }

    unsigned offsets[BUTTONS_MAX_LINES];
    build_offsets_array(map, map_count, offsets);

    int ufd = uinput_open();
    if (ufd < 0) {
        fprintf(stderr, "uinput open failed: %s\n", strerror(-ufd));
        return 1;
    }

    int keycodes[BUTTONS_MAX_LINES];
    for (size_t i = 0; i < map_count; i++) keycodes[i] = map[i].keycode;
    if (uinput_setup_keyboard(ufd, keycodes, map_count) != 0) {
        fprintf(stderr, "uinput setup failed.\n");
        close(ufd);
        return 1;
    }

    struct app_ctx app;
    memset(&app, 0, sizeof(app));
    app.ufd = ufd;
    app.map = map;
    app.map_count = map_count;
    app.min_gap_ms = min_gap_ms;
    for (size_t i = 0; i < BUTTONS_MAX_LINES; i++) {
        app.st[i].last_level = -1;
        app.st[i].last_ts_ns = 0;
    }

    if (buttons_gpio_open(&app.gpio, chip, offsets, map_count, active_low, debounce_ms, 64) != 0) {
        fprintf(stderr, "gpio open failed.\n");
        ioctl(ufd, UI_DEV_DESTROY);
        close(ufd);
        return 1;
    }

    for (;;) {
        int r = buttons_gpio_poll(app.gpio, 1000, on_gpio_event, &app);
        if (r < 0) {
            // transient errors may occur; break on fatal
            fprintf(stderr, "poll error: %d\n", r);
            break;
        }
    }

    buttons_gpio_close(app.gpio);
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    return 0;
}
