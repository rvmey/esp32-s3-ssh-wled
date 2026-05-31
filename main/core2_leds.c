#include "core2_leds.h"

#include <math.h>
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c2leds";

#define RMT_RESOLUTION_HZ  10000000  /* 10 MHz */

static led_strip_handle_t s_strip = NULL;

void core2_leds_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num         = CORE2_LED_GPIO,
        .max_leds               = CORE2_LED_COUNT,
        .led_model              = LED_MODEL_SK6812,        /* Core2 for AWS uses SK6812 (RGBW) */
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRBW,
        .flags.invert_out       = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = RMT_RESOLUTION_HZ,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        s_strip = NULL;
        return;
    }

    /* Brief boot flash — all white for 300 ms — confirms wiring before first use. */
    for (int i = 0; i < CORE2_LED_COUNT; i++) {
        led_strip_set_pixel(s_strip, (uint32_t)i, 30, 30, 30);
    }
    err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot flash refresh failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
    ESP_LOGI(TAG, "WS2812 x%d initialised on GPIO %d", CORE2_LED_COUNT, CORE2_LED_GPIO);
}

bool core2_leds_initialized(void)
{
    return s_strip != NULL;
}

/* Maps band index 0..9 to an RGB hue: red (bass) → violet (treble). */
static void band_to_rgb(int band, float level, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* hue 0° (red) → 270° (violet) across 10 bands */
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
    if (!s_strip) {
        ESP_LOGE(TAG, "set_solid: strip not initialized");
        return;
    }
    for (int i = 0; i < CORE2_LED_COUNT; i++) {
        led_strip_set_pixel(s_strip, (uint32_t)i, r, g, b);
    }
    esp_err_t err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_solid refresh failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "set_solid rgb(%u,%u,%u) OK", r, g, b);
    }
}

void core2_leds_set_bands(const float *levels, int count)
{
    if (!s_strip) return;
    if (count > CORE2_LED_COUNT) count = CORE2_LED_COUNT;
    for (int i = 0; i < CORE2_LED_COUNT; i++) {
        float lv = (i < count) ? levels[i] : 0.0f;
        if (lv < 0.0f) lv = 0.0f;
        if (lv > 1.0f) lv = 1.0f;
        uint8_t r, g, b;
        band_to_rgb(i, lv, &r, &g, &b);
        led_strip_set_pixel(s_strip, (uint32_t)i, r, g, b);
    }
    esp_err_t err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "refresh failed: %s", esp_err_to_name(err));
    }
}

void core2_leds_off(void)
{
    if (!s_strip) return;
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
}
