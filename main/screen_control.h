#pragma once

#include "esp_err.h"
#include <stdbool.h>
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
 *        Text wraps at word boundaries; column/row capacity depends on
 *        the current orientation (portrait: 20 cols × 30 rows;
 *        landscape: 30 cols × 20 rows).
 *
 * @param text  NUL-terminated ASCII string (printable 0x20–0x7E).
 */
void screen_draw_text(const char *text);

/**
 * @brief Switch between portrait (320×480) and landscape (480×320) mode.
 *        Sends MADCTL to the display, then redraws the background colour.
 *
 * @param landscape  true = landscape (480 wide, 320 tall),
 *                   false = portrait  (320 wide, 480 tall).
 */
void screen_set_landscape(bool landscape);

/**
 * @brief Return the current orientation.
 *
 * @param landscape  Set to true if currently in landscape mode.
 */
void screen_get_landscape(bool *landscape);

/**
 * @brief Set the font scale factor used by screen_draw_text().
 *        Each font pixel is rendered as (scale × scale) screen pixels.
 *        Valid range: 1–4.  Default: 2 (16×16 px per character).
 *
 * @param scale  Scale factor 1–4 (clamped if out of range).
 */
void screen_set_font_scale(int scale);

/**
 * @brief Return the current font scale factor.
 *
 * @param scale  Output: current scale (1–4).
 */
void screen_get_font_scale(int *scale);
