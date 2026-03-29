#include "led_control.h"

#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* GPIO 48 is the WS2812 on the ESP32-S3-DevKitC-1 */
#define LED_GPIO       48
#define LED_COUNT      1
#define RMT_RESOLUTION 10000000  /* 10 MHz */

static const char *TAG = "led_ctrl";

static led_strip_handle_t s_strip;
static uint8_t            s_r, s_g, s_b;
static SemaphoreHandle_t  s_mutex;

/* ------------------------------------------------------------------ */

void led_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    led_strip_config_t strip_cfg = {
        .strip_gpio_num          = LED_GPIO,
        .max_leds                = LED_COUNT,
        .led_model               = LED_MODEL_WS2812,
        .color_component_format  = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out        = false,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));

    /* Start with LED off */
    led_off();
    ESP_LOGI(TAG, "WS2812 initialised on GPIO %d", LED_GPIO);
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_r = r;
    s_g = g;
    s_b = b;

    ESP_ERROR_CHECK(led_strip_set_pixel(s_strip, 0, r, g, b));
    ESP_ERROR_CHECK(led_strip_refresh(s_strip));

    ESP_LOGI(TAG, "Color -> R:%3d G:%3d B:%3d", r, g, b);

    xSemaphoreGive(s_mutex);
}

void led_off(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_r = s_g = s_b = 0;

    ESP_ERROR_CHECK(led_strip_clear(s_strip));
    ESP_ERROR_CHECK(led_strip_refresh(s_strip));

    xSemaphoreGive(s_mutex);
}

void led_get_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *r = s_r;
    *g = s_g;
    *b = s_b;
    xSemaphoreGive(s_mutex);
}
