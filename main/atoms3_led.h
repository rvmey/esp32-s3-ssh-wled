#pragma once

#include <stdint.h>

/** Initialise the WS2812 LED on GPIO 35 (M5Stack AtomS3 Lite). */
void atoms3_led_init(void);

/** Set the LED to an RGB colour (0-255 each channel). */
void atoms3_led_set(uint8_t r, uint8_t g, uint8_t b);

/** Turn the LED off. */
void atoms3_led_off(void);
