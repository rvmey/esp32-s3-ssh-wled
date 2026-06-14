#include "core2_leds.h"

#include <string.h>
#include <math.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_rom_gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c2leds";

/*
 * Direct RMT driver for SK6812 (GRBW, 32 bits/LED).
 *
 * Timing at 10 MHz resolution (100 ns per tick).
 * Using WS2812-compatible widened timing which most SK6812 clones accept:
 *   T0H = 400 ns (4 ticks)   T0L = 800 ns (8 ticks)
 *   T1H = 800 ns (8 ticks)   T1L = 400 ns (4 ticks)
 * Reset: idle LOW between transmissions (always >> 80 μs at our call rate).
 */
#define SK6812_RMT_RES_HZ    10000000U
#define SK6812_T0H_TICKS     4
#define SK6812_T0L_TICKS     8
#define SK6812_T1H_TICKS     8
#define SK6812_T1L_TICKS     4

/* Pixel buffer: GRB order, 3 bytes per LED (SK6812 on M5Core2 is 24-bit, no W) */
static uint8_t s_pixels[CORE2_LED_COUNT * 3];

static rmt_channel_handle_t s_rmt_chan = NULL;
static rmt_encoder_handle_t s_encoder  = NULL;

static esp_err_t led_flush(void)
{
    if (!s_rmt_chan || !s_encoder) return ESP_ERR_INVALID_STATE;
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_rmt_chan, s_encoder, s_pixels, sizeof(s_pixels), &tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_transmit: %s", esp_err_to_name(err));
        return err;
    }
    err = rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(500));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_tx_wait: %s", esp_err_to_name(err));
    }
    return err;
}

void core2_leds_init(void)
{
    memset(s_pixels, 0, sizeof(s_pixels));

    /* GPIO 25 is also DAC1/RTCIO on classic ESP32.
     * 1. Release from RTC subsystem (allows GPIO matrix to take over the pad)
     * 2. Force GPIO matrix function on the pad
     * 3. Drive LOW briefly to satisfy SK6812 reset requirement before RMT */
    rtc_gpio_deinit((gpio_num_t)CORE2_LED_GPIO);
    esp_rom_gpio_pad_select_gpio(CORE2_LED_GPIO);
    gpio_set_direction((gpio_num_t)CORE2_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability((gpio_num_t)CORE2_LED_GPIO, GPIO_DRIVE_CAP_3);
    gpio_set_level((gpio_num_t)CORE2_LED_GPIO, 0);
    ESP_LOGI(TAG, "GPIO %d level before RMT: %d", CORE2_LED_GPIO,
             gpio_get_level((gpio_num_t)CORE2_LED_GPIO));

    vTaskDelay(pdMS_TO_TICKS(1));  /* >80 μs LOW for SK6812 reset */

    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num          = CORE2_LED_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = SK6812_RMT_RES_HZ,
        .mem_block_symbols = 512,
        .trans_queue_depth = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &s_rmt_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel: %s", esp_err_to_name(err));
        return;
    }

    rmt_bytes_encoder_config_t enc_cfg = {0};
    enc_cfg.bit0.level0    = 1;
    enc_cfg.bit0.duration0 = SK6812_T0H_TICKS;
    enc_cfg.bit0.level1    = 0;
    enc_cfg.bit0.duration1 = SK6812_T0L_TICKS;
    enc_cfg.bit1.level0    = 1;
    enc_cfg.bit1.duration0 = SK6812_T1H_TICKS;
    enc_cfg.bit1.level1    = 0;
    enc_cfg.bit1.duration1 = SK6812_T1L_TICKS;
    enc_cfg.flags.msb_first = 1;

    err = rmt_new_bytes_encoder(&enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_bytes_encoder: %s", esp_err_to_name(err));
        rmt_del_channel(s_rmt_chan);
        s_rmt_chan = NULL;
        return;
    }

    err = rmt_enable(s_rmt_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable: %s", esp_err_to_name(err));
        rmt_del_encoder(s_encoder);
        rmt_del_channel(s_rmt_chan);
        s_encoder  = NULL;
        s_rmt_chan = NULL;
        return;
    }

    ESP_LOGI(TAG, "GPIO %d level after rmt_enable: %d", CORE2_LED_GPIO,
             gpio_get_level((gpio_num_t)CORE2_LED_GPIO));

    /* Boot flash: dim white for 300 ms — confirms wiring on each boot. */
    for (int i = 0; i < CORE2_LED_COUNT; i++) {
        s_pixels[i * 3 + 0] = 30; /* G */
        s_pixels[i * 3 + 1] = 30; /* R */
        s_pixels[i * 3 + 2] = 30; /* B */
    }
    esp_err_t flash_err = led_flush();
    ESP_LOGI(TAG, "boot flash: %s  GPIO level after: %d",
             flash_err == ESP_OK ? "OK" : esp_err_to_name(flash_err),
             gpio_get_level((gpio_num_t)CORE2_LED_GPIO));

    vTaskDelay(pdMS_TO_TICKS(300));
    memset(s_pixels, 0, sizeof(s_pixels));
    led_flush();

    ESP_LOGI(TAG, "SK6812 x%d on GPIO %d (direct RMT, WS2812 timing)",
             CORE2_LED_COUNT, CORE2_LED_GPIO);
}

bool core2_leds_initialized(void)
{
    return s_rmt_chan != NULL && s_encoder != NULL;
}

/* Maps band 0..9 to an RGB hue: red (bass) → violet (treble). */
static void band_to_rgb(int band, float level, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float hue = (float)band / (float)(CORE2_LED_COUNT - 1) * 270.0f;
    float h   = hue / 60.0f;
    float x   = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f);
    float r1, g1, b1;
    if      (h < 1.0f) { r1 = 1.0f; g1 = x;    b1 = 0.0f; }
    else if (h < 2.0f) { r1 = x;    g1 = 1.0f; b1 = 0.0f; }
    else if (h < 3.0f) { r1 = 0.0f; g1 = 1.0f; b1 = x;    }
    else if (h < 4.0f) { r1 = 0.0f; g1 = x;    b1 = 1.0f; }
    else               { r1 = x;    g1 = 0.0f; b1 = 1.0f; }
    *r = (uint8_t)(r1 * level * 200.0f);
    *g = (uint8_t)(g1 * level * 200.0f);
    *b = (uint8_t)(b1 * level * 200.0f);
}

void core2_leds_set_solid(uint8_t r, uint8_t g, uint8_t b)
{
    if (!core2_leds_initialized()) {
        ESP_LOGE(TAG, "set_solid: not initialized");
        return;
    }
    for (int i = 0; i < CORE2_LED_COUNT; i++) {
        s_pixels[i * 3 + 0] = g;
        s_pixels[i * 3 + 1] = r;
        s_pixels[i * 3 + 2] = b;
    }
    esp_err_t err = led_flush();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "set_solid rgb(%u,%u,%u) OK  GPIO level: %d",
                 r, g, b, gpio_get_level((gpio_num_t)CORE2_LED_GPIO));
    }
}

void core2_leds_set_bands(const float *levels, int count)
{
    if (!core2_leds_initialized()) return;
    if (count > CORE2_LED_COUNT) count = CORE2_LED_COUNT;
    for (int i = 0; i < CORE2_LED_COUNT; i++) {
        float lv = (i < count) ? levels[i] : 0.0f;
        if (lv < 0.0f) lv = 0.0f;
        if (lv > 1.0f) lv = 1.0f;
        uint8_t r, g, b;
        band_to_rgb(i, lv, &r, &g, &b);
        s_pixels[i * 3 + 0] = g;
        s_pixels[i * 3 + 1] = r;
        s_pixels[i * 3 + 2] = b;
    }
    esp_err_t err = led_flush();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_bands: %s", esp_err_to_name(err));
    }
}

/* VU-meter color ramp: green for the lower portion of a zone, yellow in the
 * middle, red at the top — independent of which audio band drives it. */
static void vu_meter_rgb(int pos_in_zone, int zone_size, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float frac = (zone_size <= 1) ? 0.0f : (float)pos_in_zone / (float)(zone_size - 1);
    if (frac < 0.6f)      { *r = 0;   *g = 160; *b = 0; }
    else if (frac < 0.85f) { *r = 160; *g = 160; *b = 0; }
    else                   { *r = 160; *g = 0;   *b = 0; }
}

/* Visualizer style 1: simple VU-meter bars.
 * The first 5 LEDs fill outward (LED0 -> LED4) with the low-frequency level,
 * the last 5 LEDs fill inward (LED9 -> LED5) with the high-frequency level —
 * louder audio lights more LEDs in each row. */
void core2_leds_set_vu(float low_level, float high_level)
{
    if (!core2_leds_initialized()) return;
    if (low_level < 0.0f)  low_level  = 0.0f;
    if (low_level > 1.0f)  low_level  = 1.0f;
    if (high_level < 0.0f) high_level = 0.0f;
    if (high_level > 1.0f) high_level = 1.0f;

    const int zone = CORE2_LED_COUNT / 2; /* 5 */
    int low_n  = (int)(low_level  * zone + 0.5f);
    int high_n = (int)(high_level * zone + 0.5f);
    if (low_n  > zone) low_n  = zone;
    if (high_n > zone) high_n = zone;

    for (int i = 0; i < zone; i++) {
        uint8_t r = 0, g = 0, b = 0;
        if (i < low_n) vu_meter_rgb(i, zone, &r, &g, &b);
        s_pixels[i * 3 + 0] = g;
        s_pixels[i * 3 + 1] = r;
        s_pixels[i * 3 + 2] = b;
    }
    for (int i = 0; i < zone; i++) {
        int led = CORE2_LED_COUNT - 1 - i; /* fills LED9 down to LED5 */
        uint8_t r = 0, g = 0, b = 0;
        if (i < high_n) vu_meter_rgb(i, zone, &r, &g, &b);
        s_pixels[led * 3 + 0] = g;
        s_pixels[led * 3 + 1] = r;
        s_pixels[led * 3 + 2] = b;
    }

    esp_err_t err = led_flush();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_vu: %s", esp_err_to_name(err));
    }
}

/* Visualizer style 5: VU-meter bars, bottom-up.
 * Same low/high band split and green/yellow/red ramp as style 1, but fills
 * from the bottom corners (LED4 left, LED5 right) toward the top corners
 * (LED0 left, LED9 right) as the level rises — the opposite fill direction
 * from style 1. */
void core2_leds_set_vu_bottomup(float low_level, float high_level)
{
    if (!core2_leds_initialized()) return;
    if (low_level < 0.0f)  low_level  = 0.0f;
    if (low_level > 1.0f)  low_level  = 1.0f;
    if (high_level < 0.0f) high_level = 0.0f;
    if (high_level > 1.0f) high_level = 1.0f;

    const int zone = CORE2_LED_COUNT / 2; /* 5 */
    int low_n  = (int)(low_level  * zone + 0.5f);
    int high_n = (int)(high_level * zone + 0.5f);
    if (low_n  > zone) low_n  = zone;
    if (high_n > zone) high_n = zone;

    for (int i = 0; i < zone; i++) {
        int led = (zone - 1) - i; /* fills LED4 up to LED0 */
        uint8_t r = 0, g = 0, b = 0;
        if (i < low_n) vu_meter_rgb(i, zone, &r, &g, &b);
        s_pixels[led * 3 + 0] = g;
        s_pixels[led * 3 + 1] = r;
        s_pixels[led * 3 + 2] = b;
    }
    for (int i = 0; i < zone; i++) {
        int led = zone + i; /* fills LED5 up to LED9 */
        uint8_t r = 0, g = 0, b = 0;
        if (i < high_n) vu_meter_rgb(i, zone, &r, &g, &b);
        s_pixels[led * 3 + 0] = g;
        s_pixels[led * 3 + 1] = r;
        s_pixels[led * 3 + 2] = b;
    }

    esp_err_t err = led_flush();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_vu_bottomup: %s", esp_err_to_name(err));
    }
}

/* Visualizer style 4: mirrored VU meter — both side faces fill outward from
 * the bottom together, driven by the same overall loudness level (0..5 LEDs
 * per side). The boundary LED is partially lit (brightness scaled by the
 * fractional part of the level) for a smooth gradient. All lit LEDs are red.
 *
 * On the physical hardware LED4 (left) / LED5 (right) sit at the bottom
 * corners and LED0 (left) / LED9 (right) sit at the top corners (the
 * opposite of the indexing used by styles 1/2), so the fill grows
 * LED4 -> LED0 and LED5 -> LED9 as loudness increases. */
void core2_leds_set_vu_mirror(float level)
{
    if (!core2_leds_initialized()) return;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    const int zone = CORE2_LED_COUNT / 2; /* 5 */
    float pos  = level * (float)zone;
    int   full = (int)pos;
    if (full > zone) full = zone;
    float frac = pos - (float)full;

    for (int i = 0; i < zone; i++) {
        uint8_t br = 0;
        if (i < full) {
            br = 160;
        } else if (i == full) {
            br = (uint8_t)(frac * 160.0f + 0.5f);
        }
        int left_led  = (zone - 1) - i; /* LED4 -> LED0 */
        int right_led = zone + i;       /* LED5 -> LED9 */
        s_pixels[left_led * 3 + 0] = 0;
        s_pixels[left_led * 3 + 1] = br;
        s_pixels[left_led * 3 + 2] = 0;
        s_pixels[right_led * 3 + 0] = 0;
        s_pixels[right_led * 3 + 1] = br;
        s_pixels[right_led * 3 + 2] = 0;
    }

    esp_err_t err = led_flush();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_vu_mirror: %s", esp_err_to_name(err));
    }
}

/* Visualizer style 6: VU bars, mixed top-down/bottom-up.
 * Highs (blue) fill downward from the top corners (LED0 left / LED9 right);
 * lows (red) fill upward from the bottom corners (LED4 left / LED5 right).
 * Both fills are applied to both side faces (mirrored), so as the music
 * gets louder the blue and red fills grow toward each other and mix into
 * magenta in the middle of each side. */
void core2_leds_set_vu_mix(float low_level, float high_level)
{
    if (!core2_leds_initialized()) return;
    if (low_level < 0.0f)  low_level  = 0.0f;
    if (low_level > 1.0f)  low_level  = 1.0f;
    if (high_level < 0.0f) high_level = 0.0f;
    if (high_level > 1.0f) high_level = 1.0f;

    const int zone = CORE2_LED_COUNT / 2; /* 5 */
    int high_n = (int)(high_level * zone + 0.5f);
    int low_n  = (int)(low_level  * zone + 0.5f);
    if (high_n > zone) high_n = zone;
    if (low_n  > zone) low_n  = zone;

    for (int i = 0; i < zone; i++) {
        bool blue = (i < high_n);          /* top-down */
        bool red  = (i >= zone - low_n);   /* bottom-up */
        uint8_t r = red  ? 160 : 0;
        uint8_t b = blue ? 160 : 0;

        int left_led  = i;                   /* LED0 (top) -> LED4 (bottom) */
        int right_led = (2 * zone - 1) - i;  /* LED9 (top) -> LED5 (bottom) */

        s_pixels[left_led * 3 + 0] = 0;
        s_pixels[left_led * 3 + 1] = r;
        s_pixels[left_led * 3 + 2] = b;
        s_pixels[right_led * 3 + 0] = 0;
        s_pixels[right_led * 3 + 1] = r;
        s_pixels[right_led * 3 + 2] = b;
    }

    esp_err_t err = led_flush();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_vu_mix: %s", esp_err_to_name(err));
    }
}

/* Visualizer style 3: chase — lights a single LED at a time, advancing
 * 0 -> 9 then wrapping back to 0 with a new color each lap. */
void core2_leds_set_chase(int position, uint8_t r, uint8_t g, uint8_t b)
{
    if (!core2_leds_initialized()) return;
    int pos = position % CORE2_LED_COUNT;
    if (pos < 0) pos += CORE2_LED_COUNT;

    memset(s_pixels, 0, sizeof(s_pixels));
    s_pixels[pos * 3 + 0] = g;
    s_pixels[pos * 3 + 1] = r;
    s_pixels[pos * 3 + 2] = b;

    esp_err_t err = led_flush();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_chase: %s", esp_err_to_name(err));
    }
}

void core2_leds_off(void)
{
    if (!core2_leds_initialized()) return;
    memset(s_pixels, 0, sizeof(s_pixels));
    led_flush();
}

/* General HSV -> RGB conversion shared by the picture_frame.c visualizer
 * styles. hue_deg wraps to 0..360, sat/val are clamped to 0..1. Output is
 * scaled to 0..200 to match the brightness ceiling used elsewhere in this
 * file (band_to_rgb, vu_meter_rgb, etc.). */
void core2_leds_hsv_to_rgb(float hue_deg, float sat, float val, uint8_t *r, uint8_t *g, uint8_t *b)
{
    hue_deg = fmodf(hue_deg, 360.0f);
    if (hue_deg < 0.0f) hue_deg += 360.0f;
    if (sat < 0.0f) sat = 0.0f;
    if (sat > 1.0f) sat = 1.0f;
    if (val < 0.0f) val = 0.0f;
    if (val > 1.0f) val = 1.0f;

    float h = hue_deg / 60.0f;
    float c = val * sat;
    float x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f));
    float m = val - c;
    float r1, g1, b1;
    if      (h < 1.0f) { r1 = c; g1 = x; b1 = 0.0f; }
    else if (h < 2.0f) { r1 = x; g1 = c; b1 = 0.0f; }
    else if (h < 3.0f) { r1 = 0.0f; g1 = c; b1 = x; }
    else if (h < 4.0f) { r1 = 0.0f; g1 = x; b1 = c; }
    else if (h < 5.0f) { r1 = x; g1 = 0.0f; b1 = c; }
    else               { r1 = c; g1 = 0.0f; b1 = x; }

    *r = (uint8_t)((r1 + m) * 200.0f);
    *g = (uint8_t)((g1 + m) * 200.0f);
    *b = (uint8_t)((b1 + m) * 200.0f);
}

/* Writes a full CORE2_LED_COUNT*3 R,G,B buffer to the strip, converting to
 * the GRB pixel order used by s_pixels. Used by the picture_frame.c
 * visualizer styles that compute their own per-LED colors. */
void core2_leds_set_pixels_rgb(const uint8_t *rgb)
{
    if (!core2_leds_initialized()) return;
    for (int i = 0; i < CORE2_LED_COUNT; i++) {
        s_pixels[i * 3 + 0] = rgb[i * 3 + 1]; /* G */
        s_pixels[i * 3 + 1] = rgb[i * 3 + 0]; /* R */
        s_pixels[i * 3 + 2] = rgb[i * 3 + 2]; /* B */
    }
    esp_err_t err = led_flush();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_pixels_rgb: %s", esp_err_to_name(err));
    }
}
