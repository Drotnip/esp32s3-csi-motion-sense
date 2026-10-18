#pragma once
#include <stdint.h>

// Onboard WS2812B status LED.
void led_init(void);
void led_set(uint8_t r, uint8_t g, uint8_t b);
static inline void led_off(void) { led_set(0, 0, 0); }
