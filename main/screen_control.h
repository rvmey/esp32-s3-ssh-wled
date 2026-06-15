#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SCREEN_GESTURE_TAP,
    SCREEN_GESTURE_SWIPE_LEFT,
    SCREEN_GESTURE_SWIPE_RIGHT,
    SCREEN_GESTURE_SWIPE_UP,
    SCREEN_GESTURE_SWIPE_DOWN,
    SCREEN_GESTURE_LONG_PRESS,
} screen_gesture_t;

typedef bool (*screen_touch_handler_t)(int x, int y, screen_gesture_t gesture);

typedef enum {
    SCREEN_PINCH_BEGIN,
    SCREEN_PINCH_MOVE,
    SCREEN_PINCH_END,
} screen_pinch_phase_t;

typedef void (*screen_pinch_handler_t)(screen_pinch_phase_t phase, int x1, int y1, int x2, int y2);

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
 * @brief Blit a pre-decoded RGB565 image to the physical panel.
 *        Scales the source image to fill the full 320×480 panel using
 *        nearest-neighbour interpolation.
 *
 * @param rgb565  Pointer to source pixels in RGB565 format (little-endian
 *                16-bit values, i.e. [LSByte, MSByte] per pixel).
 * @param src_w   Source image width in pixels.
 * @param src_h   Source image height in pixels.
 */
void screen_draw_rgb565(const uint8_t *rgb565, int src_w, int src_h);

/**
 * @brief Set the font scale factor used by screen_draw_text().
 *        Each font pixel is rendered as (scale × scale) screen pixels.
 *        Valid range: 1–4.  Default: 2 (16×16 px per character).
 *
 * @param scale  Scale factor 1–4 (clamped if out of range).
 */
void screen_set_font_scale(int scale);

/**
 * @brief Set the font scale without redrawing existing text.
 *        Use this to update the scale that will apply to the *next* draw call
 *        without causing an intermediate redraw of whatever is currently shown.
 */
void screen_set_font_scale_silent(int scale);

/**
 * @brief Return the current font scale factor.
 *
 * @param scale  Output: current scale (1–4).
 */
void screen_get_font_scale(int *scale);

/**
 * @brief Set the text (foreground) colour used by screen_draw_text().
 *        Default is white (255, 255, 255).
 *
 * @param r  Red   component 0-255
 * @param g  Green component 0-255
 * @param b  Blue  component 0-255
 */
void screen_set_text_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Return the current text colour through the output parameters.
 */
void screen_get_text_color(uint8_t *r, uint8_t *g, uint8_t *b);

/**
 * @brief Acquire exclusive access to the screen SPI path.
 *        Intended for short critical sections where another peripheral
 *        (for example SD over shared SPI lines) must probe without LCD
 *        traffic.
 *
 * @param timeout_ms  Lock timeout in milliseconds.
 *
 * @return true if lock acquired, false on timeout/error.
 */
bool screen_spi_lock(uint32_t timeout_ms);

/**
 * @brief Release exclusive access previously acquired via screen_spi_lock().
 */
void screen_spi_unlock(void);

/**
 * @brief Re-assert the display controller's output-enable state after the
 *        shared SPI bus has been used by another peripheral (e.g. SD card).
 *        On Core2 this resends SLEEP_OUT + key ILI9342C display-control
 *        registers to recover from any spurious DISPOFF or SLPIN the panel
 *        may have received during SD initialisation.  On other hardware
 *        (JC3248W535) this is a no-op.
 */
void screen_reinit_display(void);

/**
 * @brief Cut the display backlight power rail before entering deep sleep.
 *        On Core2 this clears the AXP192 DC3 enable bit.  On other hardware
 *        this is a no-op.  screen_init() restores the backlight on next boot.
 */
void screen_backlight_off(void);

/**
 * @brief Register a logical-coordinate touch callback.
 *
 * The callback receives x/y in current logical orientation coordinates
 * (portrait: 320x480, landscape: 480x320). Return true when the touch
 * was handled and should not be used for text scrolling.
 */
void screen_set_touch_handler(screen_touch_handler_t handler);

/**
 * @brief Register a two-finger pinch/zoom callback (Core2 only; the
 *        FT6336U reports up to 2 simultaneous touch points). BEGIN fires
 *        when a 2nd finger is detected, MOVE on each subsequent poll while
 *        both fingers remain down, END once fewer than 2 fingers remain
 *        (x1/y1/x2/y2 are 0 for END). Coordinates are in current
 *        logical-orientation space. No-op on hardware without multi-touch.
 */
void screen_set_pinch_handler(screen_pinch_handler_t handler);

/**
 * @brief Blit a sub-region of a pre-decoded RGB565 image to the screen,
 *        scaled to fit using the same letterbox/aspect behaviour as
 *        screen_draw_rgb565(), but treating (crop_x, crop_y, crop_w, crop_h)
 *        as the source rectangle instead of the full image. The full
 *        image's width (src_w) is used as the row stride.
 */
void screen_draw_rgb565_region(const uint8_t *rgb565, int src_w, int src_h,
                                int crop_x, int crop_y, int crop_w, int crop_h);
