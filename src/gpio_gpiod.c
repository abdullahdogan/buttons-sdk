#include "gpio_backend.h"
#include <gpiod.h>
#include <pthread.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>

#define MAX_GPIO 64

typedef struct {
    int in_use;
    unsigned gpio;
    struct gpiod_line_request *req;  /* v2: line yerine request tutuyoruz */
    gpio_alert_cb cb;
    void *userdata;
} reg_t;

static struct gpiod_chip *chip = NULL;
static reg_t regs[MAX_GPIO];
static pthread_t th;
static volatile int running = 0;

/* pull bias'ı hatırlayalım: 0 off, 1 up, 2 down */
static int bias_pref[MAX_GPIO];

static int gpio_to_offset(unsigned gpio){ return (int)gpio; }

static void* monitor_thread(void *arg){
    (void)arg;
    while (running){
        struct pollfd fds[MAX_GPIO];
        int idxmap[MAX_GPIO];
        int nfds = 0;

        /* Her aktif request için fd topla */
        for (int i=0; i<MAX_GPIO; i++){
            if (regs[i].in_use && regs[i].req){
                int fd = gpiod_line_request_get_fd(regs[i].req);
                if (fd >= 0){
                    fds[nfds].fd = fd;
                    fds[nfds].events = POLLIN;
                    fds[nfds].revents = 0;
                    idxmap[nfds] = i;
                    nfds++;
                }
            }
        }

        int to_ms = 100; /* 100ms */
        int rc = (nfds > 0) ? poll(fds, nfds, to_ms) : 0;
        if (rc <= 0) continue;

        for (int k=0; k<nfds; k++){
            if (fds[k].revents & POLLIN){
                int i = idxmap[k];

                /* v2: edge-event buffer ile oku */
                struct gpiod_edge_event_buffer *buf = gpiod_edge_event_buffer_new(8);
                if (!buf) continue;

                int r = gpiod_line_request_read_edge_events(regs[i].req, buf, 8);
                for (int j = 0; j < r; j++){
                    const struct gpiod_edge_event *cev = gpiod_edge_event_buffer_get_event(buf, j);
                    if (!cev) continue;

                    /* Header bazı dağıtımlarda non-const bekliyor; güvenli cast */
                    int rising = (gpiod_edge_event_get_event_type((struct gpiod_edge_event*)cev)
                                  == GPIOD_EDGE_EVENT_RISING_EDGE);
                    int level = rising ? 1 : 0;

                    gpio_alert_cb cb = regs[i].cb;
                    if (cb) cb(regs[i].gpio, level, 0, regs[i].userdata);

                    gpiod_edge_event_free((struct gpiod_edge_event*)cev);
                }
                gpiod_edge_event_buffer_free(buf);
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

    for (int i=0; i<MAX_GPIO; i++){
        if (regs[i].in_use && regs[i].req){
            gpiod_line_request_release(regs[i].req);
            regs[i].req = NULL;
            regs[i].in_use = 0;
        }
    }
    gpiod_chip_close(chip);
    chip = NULL;
}

void gpio_set_mode_input(unsigned gpio){
    (void)gpio; /* v2'de line_settings ile zaten input istenir */
}

void gpio_set_pull(unsigned gpio, int pull){
    if (gpio < MAX_GPIO) bias_pref[gpio] = pull; /* 0: off, 1: up, 2: down */
}

void gpio_set_glitch_filter(unsigned gpio, unsigned us){
    (void)gpio; (void)us; /* donanımsal filtre yok; yazılımsal debounce üst katmanda */
}

void gpio_set_alert(unsigned gpio, gpio_alert_cb cb, void *userdata){
    if (gpio >= MAX_GPIO || !chip) return;

    /* Eski request varsa bırak */
    if (regs[gpio].in_use && regs[gpio].req){
        gpiod_line_request_release(regs[gpio].req);
        regs[gpio].req = NULL;
        regs[gpio].in_use = 0;
    }

    if (!cb){
        return; /* sadece kaldırdık */
    }

    int offset = gpio_to_offset(gpio);

    /* v2: settings → line_config → request_config → request */
    struct gpiod_line_settings *ls = gpiod_line_settings_new();
    if (!ls) return;

    gpiod_line_settings_set_direction(ls, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_edge_detection(ls, GPIOD_LINE_EDGE_BOTH);

    /* bias */
    if      (bias_pref[gpio] == 1) gpiod_line_settings_set_bias(ls, GPIOD_LINE_BIAS_PULL_UP);
    else if (bias_pref[gpio] == 2) gpiod_line_settings_set_bias(ls, GPIOD_LINE_BIAS_PULL_DOWN);
    else                           gpiod_line_settings_set_bias(ls, GPIOD_LINE_BIAS_DISABLED);

    struct gpiod_line_config *lcfg = gpiod_line_config_new();
    if (!lcfg){ gpiod_line_settings_free(ls); return; }
    if (gpiod_line_config_add_line_settings(lcfg, &offset, 1, ls) < 0){
        gpiod_line_config_free(lcfg);
        gpiod_line_settings_free(ls);
        return;
    }

    struct gpiod_request_config *rcfg = gpiod_request_config_new();
    if (!rcfg){
        gpiod_line_config_free(lcfg);
        gpiod_line_settings_free(ls);
        return;
    }
    gpiod_request_config_set_consumer(rcfg, "buttons-sdk");

    struct gpiod_line_request *req = gpiod_chip_request_lines(chip, rcfg, lcfg);

    gpiod_request_config_free(rcfg);
    gpiod_line_config_free(lcfg);
    gpiod_line_settings_free(ls);

    if (!req){
        return; /* request alınamadı */
    }

    regs[gpio].in_use   = 1;
    regs[gpio].gpio     = gpio;
    regs[gpio].req      = req;
    regs[gpio].cb       = cb;
    regs[gpio].userdata = userdata;
}

void gpio_delay_ms(unsigned ms){
    usleep(ms*1000u);
}

uint32_t gpio_now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec*1000u + ts.tv_nsec/1000000u);
}
