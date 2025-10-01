#ifndef GPIO_BACKEND_H
#define GPIO_BACKEND_H

#include <stdint.h>

typedef void (*gpio_alert_cb)(int gpio, int level, uint32_t tick, void *userdata);
// level: 0=LOW, 1=HIGH, 2=NOISE (mümkünse)

int  gpio_backend_init(void);
void gpio_backend_term(void);

void gpio_set_mode_input(unsigned gpio);
void gpio_set_pull(unsigned gpio, int pull); // 0=OFF, 1=UP, 2=DOWN
void gpio_set_glitch_filter(unsigned gpio, unsigned us);
void gpio_set_alert(unsigned gpio, gpio_alert_cb cb, void *userdata);

void gpio_delay_ms(unsigned ms);
uint32_t gpio_now_ms(void);

#endif
