#pragma once

#include <stdint.h>

/**
 * @brief Initialise the WS2812 LED on GPIO 48 via the RMT peripheral.
 *        Must be called once before any other led_* function.
 */
void led_init(void);

/**
 * @brief Set the LED to an RGB colour.
 *
 * @param r  Red   component 0-255
 * @param g  Green component 0-255
 * @param b  Blue  component 0-255
 */
void led_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Turn the LED off (all channels to 0).
 */
void led_off(void);

/**
 * @brief Return the current colour through the output parameters.
 */
void led_get_color(uint8_t *r, uint8_t *g, uint8_t *b);
