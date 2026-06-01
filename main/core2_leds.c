#include "core2_leds.h"

#include <string.h>
#include <math.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c2leds";

/*
 * Direct RMT driver for SK6812 (GRBW, 32 bits/LED).
 * Uses the new IDF v5+/v6 rmt_tx API directly rather than the led_strip
 * component, which does not reliably drive classic ESP32 SK6812 strips.
 *
 * Timing at 10 MHz resolution (100 ns per tick):
 *   T0H = 300 ns  (3 ticks)   T0L = 900 ns  (9 ticks)
 *   T1H = 600 ns  (6 ticks)   T1L = 600 ns  (6 ticks)
 *   Reset > 80 μs — provided by idle low between transmissions.
 */

#define SK6812_RMT_RES_HZ    10000000U
#define SK6812_T0H_TICKS     3
#define SK6812_T0L_TICKS     9
#define SK6812_T1H_TICKS     6
#define SK6812_T1L_TICKS     6

/* Pixel buffer: GRBW order, 4 bytes per LED */
static uint8_t s_pixels[CORE2_LED_COUNT * 4];

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
    return rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(500));
}

void core2_leds_init(void)
{
    memset(s_pixels, 0, sizeof(s_pixels));

    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num          = CORE2_LED_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = SK6812_RMT_RES_HZ,
        /* Classic ESP32 has 512 RMT items total (8 × 64).  Allocate 512 to
         * this channel so the entire 320-symbol SK6812 frame (10 LEDs × 32
         * bits) fits in one shot with no ping-pong refill interrupts. */
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

    /* Boot flash: dim white for 300 ms to confirm wiring. */
    for (int i = 0; i < CORE2_LED_COUNT; i++) {
        s_pixels[i * 4 + 0] = 30; /* G */
        s_pixels[i * 4 + 1] = 30; /* R */
        s_pixels[i * 4 + 2] = 30; /* B */
        s_pixels[i * 4 + 3] =  0; /* W */
    }
    led_flush();
    vTaskDelay(pdMS_TO_TICKS(300));
    memset(s_pixels, 0, sizeof(s_pixels));
    led_flush();

    ESP_LOGI(TAG, "SK6812 x%d on GPIO %d (direct RMT)", CORE2_LED_COUNT, CORE2_LED_GPIO);
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
        s_pixels[i * 4 + 0] = g;
        s_pixels[i * 4 + 1] = r;
        s_pixels[i * 4 + 2] = b;
        s_pixels[i * 4 + 3] = 0;
    }
    esp_err_t err = led_flush();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "set_solid rgb(%u,%u,%u) OK", r, g, b);
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
        s_pixels[i * 4 + 0] = g;
        s_pixels[i * 4 + 1] = r;
        s_pixels[i * 4 + 2] = b;
        s_pixels[i * 4 + 3] = 0;
    }
    esp_err_t err = led_flush();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_bands: %s", esp_err_to_name(err));
    }
}

void core2_leds_off(void)
{
    if (!core2_leds_initialized()) return;
    memset(s_pixels, 0, sizeof(s_pixels));
    led_flush();
}
