// examples/keypad-hid.c
// Virtual keyboard over GPIO using libgpiod v2 and uinput.
// Goals:
//  - No EV_REP on the virtual device (prevents auto-repeat from kernel).
//  - Software throttle: same key cannot fire more than MIN_GAP_MS apart (default 150 ms).
//  - One-shot tap: emit press then release for each accepted activation.
//  - ASCII-only comments and strings.
//
// Build: this file is meant to live in examples/ of buttons-sdk
// and compile with the existing CMakeLists.txt (target: keypad-hid).

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "buttons.h"  // must declare: buttons_gpio_open, buttons_gpio_poll, buttons_gpio_close

// ---------- time helpers ----------

static inline uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void sleep_us(unsigned us) {
    struct timespec ts;
    ts.tv_sec  = us / 1000000u;
    ts.tv_nsec = (long)((us % 1000000u) * 1000u);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { /* retry */ }
}

// ---------- logging ----------

static void dief(const char *fmt, ...) __attribute__((noreturn, format(printf,1,2)));
static void dief(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void warnf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "warn: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

// ---------- uinput ----------

struct vkbd {
    int fd;
};

static void uinput_emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    // kernel fills timestamps; setting zero is fine
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        // best effort; do not die in hard loop
        warnf("uinput write failed: %s", strerror(errno));
    }
}

static int vkbd_init(struct vkbd *kb, const char *name,
                     const uint16_t *keycodes, size_t nkeys)
{
    memset(kb, 0, sizeof(*kb));
    kb->fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (kb->fd < 0) return -errno ? -errno : -ENODEV;

    // Enable EV_KEY only (no EV_REP to avoid auto-repeat).
    if (ioctl(kb->fd, UI_SET_EVBIT, EV_KEY) < 0) return -errno;

    for (size_t i = 0; i < nkeys; ++i) {
        if (ioctl(kb->fd, UI_SET_KEYBIT, keycodes[i]) < 0) return -errno;
    }

    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_USB;
    us.id.vendor  = 0x0001;
    us.id.product = 0x0001;
    us.id.version = 0x0001;
    snprintf(us.name, sizeof(us.name), "%s", name);

    if (ioctl(kb->fd, UI_DEV_SETUP, &us) < 0) return -errno;
    if (ioctl(kb->fd, UI_DEV_CREATE) < 0) return -errno;

    // Give kernel a moment to create the device node.
    sleep_us(20000);
    return 0;
}

static void vkbd_destroy(struct vkbd *kb) {
    if (kb->fd >= 0) {
        ioctl(kb->fd, UI_DEV_DESTROY);
        close(kb->fd);
        kb->fd = -1;
    }
}

static void vkbd_tap(struct vkbd *kb, uint16_t code) {
    // one-shot tap: press then release
    uinput_emit(kb->fd, EV_KEY, code, 1);
    uinput_emit(kb->fd, EV_SYN, SYN_REPORT, 0);
    // short hold to ensure consumers see it as discrete key
    sleep_us(8000);
    uinput_emit(kb->fd, EV_KEY, code, 0);
    uinput_emit(kb->fd, EV_SYN, SYN_REPORT, 0);
}

// ---------- map parsing ----------
//
// Format: --map "PIN:NAME,PIN:NAME,..."
// NAME in {up,down,left,right,enter,esc}
// Example: --map "17:up,22:down,23:left,24:right,25:enter,27:esc"

struct map_entry {
    unsigned pin;
    uint16_t keycode;
};

struct map {
    struct map_entry *v;
    size_t n;
};

static uint16_t parse_key_name(const char *s) {
    if (!s) return 0;
    if (strcmp(s, "up")    == 0) return KEY_UP;
    if (strcmp(s, "down")  == 0) return KEY_DOWN;
    if (strcmp(s, "left")  == 0) return KEY_LEFT;
    if (strcmp(s, "right") == 0) return KEY_RIGHT;
    if (strcmp(s, "enter") == 0) return KEY_ENTER;
    if (strcmp(s, "esc")   == 0) return KEY_ESC;
    return 0;
}

static void map_free(struct map *m) {
    free(m->v);
    m->v = NULL;
    m->n = 0;
}

static void map_parse(struct map *m, const char *spec) {
    // copy input to a temp buffer to tokenise
    char *buf = strdup(spec ? spec : "");
    if (!buf) dief("oom");
    size_t cap = 0;
    m->v = NULL; m->n = 0;

    char *saveptr = NULL;
    for (char *tok = strtok_r(buf, ",", &saveptr);
         tok;
         tok = strtok_r(NULL, ",", &saveptr)) {
        // trim spaces
        while (*tok == ' ' || *tok == '\t') tok++;
        if (*tok == 0) continue;

        char *colon = strchr(tok, ':');
        if (!colon) dief("bad map token: %s (expected PIN:NAME)", tok);
        *colon = 0;
        const char *pins = tok;
        const char *names = colon + 1;

        char *endp = NULL;
        long pin = strtol(pins, &endp, 10);
        if (endp == pins || pin < 0 || pin > 1023) dief("bad pin: %s", pins);

        uint16_t code = parse_key_name(names);
        if (!code) dief("bad key name: %s", names);

        if (m->n == cap) {
            cap = cap ? cap * 2 : 8;
            struct map_entry *nv = realloc(m->v, cap * sizeof(*nv));
            if (!nv) dief("oom");
            m->v = nv;
        }
        m->v[m->n].pin = (unsigned)pin;
        m->v[m->n].keycode = code;
        m->n += 1;
    }

    if (m->n == 0) {
        free(buf);
        dief("empty map");
    }
    free(buf);
}

// ---------- signal handling ----------

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

// ---------- main ----------

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [--chip NAME] [--active-low] [--debounce-ms N] [--min-gap-ms N] \\\n"
        "          --map \"PIN:NAME[,PIN:NAME,...]\"\n"
        "\n"
        "NAME in {up,down,left,right,enter,esc}\n"
        "Example:\n"
        "  %s --chip gpiochip0 --active-low --debounce-ms 30 --min-gap-ms 150 \\\n"
        "     --map \"17:up,22:down,23:left,24:right,25:enter,27:esc\"\n",
        argv0, argv0);
}

int main(int argc, char **argv) {
    const char *chip = "gpiochip0";
    bool active_low = false;
    unsigned debounce_ms = 30;   // hardware debounce at line level
    unsigned min_gap_ms = 150;   // software throttle for same key
    const char *map_spec = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--chip") == 0 && i + 1 < argc) {
            chip = argv[++i];
        } else if (strcmp(argv[i], "--active-low") == 0) {
            active_low = true;
        } else if (strcmp(argv[i], "--debounce-ms") == 0 && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if (v < 0 || v > 2000) dief("bad debounce-ms");
            debounce_ms = (unsigned)v;
        } else if (strcmp(argv[i], "--min-gap-ms") == 0 && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if (v < 0 || v > 5000) dief("bad min-gap-ms");
            min_gap_ms = (unsigned)v;
        } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            map_spec = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!map_spec) {
        usage(argv[0]);
        return 2;
    }

    struct map M = {0};
    map_parse(&M, map_spec);

    // Build arrays for backend and for vkbd capabilities
    unsigned *offsets = calloc(M.n, sizeof(unsigned));
    uint16_t *keycodes = calloc(M.n, sizeof(uint16_t));
    if (!offsets || !keycodes) dief("oom");
    for (size_t i = 0; i < M.n; ++i) {
        offsets[i] = M.v[i].pin;
        keycodes[i] = M.v[i].keycode;
    }

    // Install signal handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Init uinput vkbd
    struct vkbd kb;
    int rc = vkbd_init(&kb, "Keypad HID (buttons-sdk)", keycodes, M.n);
    if (rc != 0) dief("uinput init failed: %s", strerror(-rc));

    // Open GPIO backend
    rc = buttons_gpio_open(chip, offsets, M.n, active_low, debounce_ms);
    if (rc != 0) dief("gpio open failed on %s: %s", chip, strerror(-rc));

    const uint64_t min_gap_ns = (uint64_t)min_gap_ms * 1000000ull;
    uint64_t *last_fire_ns = calloc(M.n, sizeof(uint64_t));
    if (!last_fire_ns) dief("oom");

    fprintf(stderr, "ready: chip=%s active_low=%d debounce_ms=%u min_gap_ms=%u\n",
            chip, active_low ? 1 : 0, debounce_ms, min_gap_ms);

    // Main loop
    while (!g_stop) {
        unsigned idx = 0;
        int edge = 0;
        int r = buttons_gpio_poll(1000, &idx, &edge);
        if (r < 0) {
            // transient errors: continue
            warnf("poll error: %s", strerror(-r));
            continue;
        }
        if (r == 0) {
            // timeout, loop
            continue;
        }

        // edge is 1 for rising, 0 for falling (from backend).
        // We treat any edge as activation candidate and throttle by min_gap.
        if (idx >= M.n) {
            // should not happen; ignore
            continue;
        }

        uint64_t now = mono_ns();
        if (now - last_fire_ns[idx] < min_gap_ns) {
            // too soon; ignore duplicate edge
            continue;
        }

        // Fire one-shot tap for the mapped key
        vkbd_tap(&kb, keycodes[idx]);
        last_fire_ns[idx] = now;
    }

    // Cleanup
    buttons_gpio_close();
    vkbd_destroy(&kb);
    free(last_fire_ns);
    free(offsets);
    free(keycodes);
    map_free(&M);
    return 0;
}
