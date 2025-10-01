#include "gpio_backend.h"
#include <gpiod.h>
#include <pthread.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>

#define MAX_GPIO 64

typedef struct {
    int in_use;
    unsigned gpio;
    struct gpiod_line *line;
    gpio_alert_cb cb;
    void *userdata;
} reg_t;

static struct gpiod_chip *chip = NULL;
static reg_t regs[MAX_GPIO];
static pthread_t th;
static volatile int running = 0;

// pull bias'ı hatırlayalım: 0 off, 1 up, 2 down
static int bias_pref[MAX_GPIO];

static int gpio_to_offset(unsigned gpio){ return (int)gpio; }

static void* monitor_thread(void *arg){
    (void)arg;
    while (running){
        struct pollfd fds[MAX_GPIO];
        int idxmap[MAX_GPIO];
        int nfds = 0;

        for (int i=0;i<MAX_GPIO;i++){
            if (regs[i].in_use && regs[i].line){
                int fd = gpiod_line_event_get_fd(regs[i].line);
                if (fd >= 0){
                    fds[nfds].fd = fd;
                    fds[nfds].events = POLLIN;
                    idxmap[nfds] = i;
                    nfds++;
                }
            }
        }

        int to_ms = 100; // 100ms
        int rc = poll(fds, nfds, to_ms);
        if (rc <= 0) continue;

        for (int k=0;k<nfds;k++){
            if (fds[k].revents & POLLIN){
                struct gpiod_line_event ev;
                int i = idxmap[k];
                if (gpiod_line_event_read(regs[i].line, &ev) == 0){
                    int level = (ev.event_type == GPIOD_LINE_EVENT_RISING_EDGE) ? 1 : 0;
                    gpio_alert_cb cb = regs[i].cb;
                    if (cb) cb(regs[i].gpio, level, 0, regs[i].userdata);
                }
            }
        }
    }
    return NULL;
}

int gpio_backend_init(void){
    memset(regs, 0, sizeof(regs));
    memset(bias_pref, 0, sizeof(bias_pref));
    chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip) return -1;
    running = 1;
    if (pthread_create(&th, NULL, monitor_thread, NULL) != 0){
        gpiod_chip_close(chip);
        chip = NULL;
        running = 0;
        return -1;
    }
    return 0;
}

void gpio_backend_term(void){
    if (!chip) return;
    running = 0;
    pthread_join(th, NULL);

    for (int i=0;i<MAX_GPIO;i++){
        if (regs[i].in_use && regs[i].line){
            gpiod_line_release(regs[i].line);
            regs[i].line = NULL;
            regs[i].in_use = 0;
        }
    }
    gpiod_chip_close(chip);
    chip = NULL;
}

void gpio_set_mode_input(unsigned gpio){
    (void)gpio; // libgpiod'de event request sırasında zaten input olarak alınır
}

void gpio_set_pull(unsigned gpio, int pull){
    if (gpio < MAX_GPIO) bias_pref[gpio] = pull;
}

void gpio_set_glitch_filter(unsigned gpio, unsigned us){
    (void)gpio; (void)us; // donanımsal filtre yok; yazılımsal debounce üst katmanda var
}

void gpio_set_alert(unsigned gpio, gpio_alert_cb cb, void *userdata){
    if (gpio >= MAX_GPIO || !chip) return;

    // varsa eski istek bırak
    if (regs[gpio].in_use && regs[gpio].line){
        gpiod_line_release(regs[gpio].line);
        regs[gpio].line = NULL;
        regs[gpio].in_use = 0;
    }

    if (!cb){
        return; // sadece kaldırdık
    }

    int offset = gpio_to_offset(gpio);
    struct gpiod_line *line = gpiod_chip_get_line(chip, offset);
    if (!line) return;

    struct gpiod_line_request_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.consumer = "buttons-sdk";
    cfg.request_type = GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES;
    cfg.flags = 0;

    // bias uygun ise isteyelim (çekme direnci)
#ifdef GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP
    if (bias_pref[gpio] == 1) cfg.flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP;
#endif
#ifdef GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN
    if (bias_pref[gpio] == 2) cfg.flags |= GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN;
#endif

    if (gpiod_line_request(line, &cfg, 0) == 0){
        regs[gpio].in_use = 1;
        regs[gpio].gpio = gpio;
        regs[gpio].line = line;
        regs[gpio].cb = cb;
        regs[gpio].userdata = userdata;
    } else {
        // request başarısızsa line'ı bırak
        gpiod_line_release(line);
    }
}

void gpio_delay_ms(unsigned ms){
    usleep(ms*1000u);
}

uint32_t gpio_now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec*1000u + ts.tv_nsec/1000000u);
}
