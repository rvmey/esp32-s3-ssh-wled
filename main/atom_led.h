#pragma once

#include <stdint.h>

/*
 * atom_led — SK6812 single-pixel RGB LED for the M5Stack ATOM Echo.
 *
 * The SK6812 is wired to GPIO 27 and driven via the RMT peripheral using the
 * espressif/led_strip component (same component used by led_control.c).
 */

/** Initialise the RMT channel and LED strip driver. LED starts OFF. */
void atom_led_init(void);

/** Set the LED to the given RGB colour. */
void atom_led_set(uint8_t r, uint8_t g, uint8_t b);

/** Turn the LED off. */
void atom_led_off(void);
