#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Initialise the AXS15231 QSPI display on the Guition JC3248W535.
 *        Configures the QSPI SPI bus, sends the AXS15231 init sequence,
 *        turns the backlight fully on, and clears the screen to black.
 *
 * @return ESP_OK on success.
 */
esp_err_t screen_init(void);

/**
 * @brief Fill the entire screen with a solid RGB colour.
 *
 * @param r  Red   component 0-255
 * @param g  Green component 0-255
 * @param b  Blue  component 0-255
 */
void screen_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Fill the screen with black.
 */
void screen_off(void);

/**
 * @brief Return the current screen colour through the output parameters.
 */
void screen_get_color(uint8_t *r, uint8_t *g, uint8_t *b);

/**
 * @brief Render text centred on the screen using a white 16×16-pixel font
 *        (8×8 bitmap scaled 2×) on the current background colour.
 *        Text wraps at 20 characters per row; up to 30 rows are supported.
 *
 * @param text  NUL-terminated ASCII string (printable 0x20–0x7E).
 */
void screen_draw_text(const char *text);
