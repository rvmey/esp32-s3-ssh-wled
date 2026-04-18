#include "atom_led.h"

#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* GPIO 27 is the SK6812 on the M5Stack ATOM Echo */
#define LED_GPIO        27
#define LED_COUNT       1
#define RMT_RESOLUTION  10000000  /* 10 MHz */

static const char *TAG = "atom_led";

static led_strip_handle_t s_strip;
static SemaphoreHandle_t  s_mutex;

void atom_led_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    led_strip_config_t strip_cfg = {
        .strip_gpio_num         = LED_GPIO,
        .max_leds               = LED_COUNT,
        .led_model              = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out       = false,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = RMT_RESOLUTION,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));

    atom_led_off();
    ESP_LOGI(TAG, "SK6812 initialised on GPIO %d", LED_GPIO);
}

void atom_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ESP_ERROR_CHECK(led_strip_set_pixel(s_strip, 0, r, g, b));
    ESP_ERROR_CHECK(led_strip_refresh(s_strip));
    xSemaphoreGive(s_mutex);
}

void atom_led_off(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ESP_ERROR_CHECK(led_strip_clear(s_strip));
    ESP_ERROR_CHECK(led_strip_refresh(s_strip));
    xSemaphoreGive(s_mutex);
}
