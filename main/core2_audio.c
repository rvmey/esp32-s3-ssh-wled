#include "core2_audio.h"

#include <string.h>
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CORE2_I2S_PORT      I2S_NUM_1
#define CORE2_I2S_BCLK_GPIO 12
#define CORE2_I2S_WS_GPIO   0
#define CORE2_I2S_DOUT_GPIO 2
#define CORE2_I2S_CHUNK_FRAMES 512
#define CORE2_I2S_DMA_DESC_NUM 8
#define CORE2_I2S_RING_BYTES (48 * 1024)
#define CORE2_I2S_RING_FALLBACK_BYTES (12 * 1024)

static const char *TAG = "core2_audio";

static i2s_chan_handle_t s_tx_chan = NULL;
static uint32_t s_sample_rate_hz = 44100;
static TaskHandle_t s_writer_task = NULL;
static StaticSemaphore_t s_ring_mutex_storage;
static SemaphoreHandle_t s_ring_mutex = NULL;
static uint8_t *s_ring_buf = NULL;
static size_t s_ring_capacity = 0;
static size_t s_ring_read_pos = 0;
static size_t s_ring_write_pos = 0;
static size_t s_ring_used = 0;
static volatile bool s_writer_running = false;

static void core2_audio_reset_ring_locked(void)
{
    s_ring_read_pos = 0;
    s_ring_write_pos = 0;
    s_ring_used = 0;
}

static size_t core2_audio_ring_write(const uint8_t *data, size_t len)
{
    size_t free_bytes = s_ring_capacity - s_ring_used;
    if (len > free_bytes) len = free_bytes;
    if (len == 0) return 0;

    size_t first = s_ring_capacity - s_ring_write_pos;
    if (first > len) first = len;
    memcpy(&s_ring_buf[s_ring_write_pos], data, first);
    if (len > first) {
        memcpy(s_ring_buf, data + first, len - first);
    }

    s_ring_write_pos = (s_ring_write_pos + len) % s_ring_capacity;
    s_ring_used += len;
    return len;
}

static size_t core2_audio_ring_read(uint8_t *dst, size_t len)
{
    if (len > s_ring_used) len = s_ring_used;
    if (len == 0) return 0;

    size_t first = s_ring_capacity - s_ring_read_pos;
    if (first > len) first = len;
    memcpy(dst, &s_ring_buf[s_ring_read_pos], first);
    if (len > first) {
        memcpy(dst + first, s_ring_buf, len - first);
    }

    s_ring_read_pos = (s_ring_read_pos + len) % s_ring_capacity;
    s_ring_used -= len;
    return len;
}

static void core2_audio_writer_task(void *arg)
{
    uint8_t out[CORE2_I2S_CHUNK_FRAMES * 2 * sizeof(int16_t)];
    (void)arg;

    while (s_writer_running) {
        size_t chunk = 0;

        if (xSemaphoreTake(s_ring_mutex, portMAX_DELAY) == pdTRUE) {
            chunk = s_ring_used;
            if (chunk > sizeof(out)) chunk = sizeof(out);
            chunk -= (chunk % (2 * sizeof(int16_t)));
            if (chunk > 0) {
                core2_audio_ring_read(out, chunk);
            }
            xSemaphoreGive(s_ring_mutex);
        }

        if (chunk == 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        if (!s_tx_chan) {
            continue;
        }

        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, out, chunk, &written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    s_writer_task = NULL;
    vTaskDelete(NULL);
}

static void core2_audio_start_writer_task(void)
{
    if (s_writer_task) return;
    s_writer_running = true;
    xTaskCreate(core2_audio_writer_task, "core2_i2s", 4096, NULL, 6, &s_writer_task);
}

void core2_audio_init(void)
{
    if (s_tx_chan) return;

    if (!s_ring_mutex) {
        s_ring_mutex = xSemaphoreCreateMutexStatic(&s_ring_mutex_storage);
    }
    if (!s_ring_buf) {
        s_ring_buf = heap_caps_malloc(CORE2_I2S_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_ring_capacity = CORE2_I2S_RING_BYTES;
        if (!s_ring_buf) {
            s_ring_buf = heap_caps_malloc(CORE2_I2S_RING_FALLBACK_BYTES, MALLOC_CAP_8BIT);
            s_ring_capacity = s_ring_buf ? CORE2_I2S_RING_FALLBACK_BYTES : 0;
        }
        if (!s_ring_buf) {
            ESP_LOGE(TAG, "Failed to allocate speaker PCM ring buffer");
            return;
        }
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CORE2_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = CORE2_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = CORE2_I2S_CHUNK_FRAMES;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CORE2_I2S_BCLK_GPIO,
            .ws = CORE2_I2S_WS_GPIO,
            .dout = CORE2_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    core2_audio_start_writer_task();

    ESP_LOGI(TAG, "I2S TX ready on BCK=%d WS=%d DOUT=%d @ %lu Hz",
             CORE2_I2S_BCLK_GPIO,
             CORE2_I2S_WS_GPIO,
             CORE2_I2S_DOUT_GPIO,
             (unsigned long)s_sample_rate_hz);
    ESP_LOGI(TAG, "speaker pcm ring=%lu bytes", (unsigned long)s_ring_capacity);
}

void core2_audio_deinit(void)
{
    if (!s_tx_chan) return;
    s_writer_running = false;
    if (s_writer_task) {
        xTaskNotifyGive(s_writer_task);
        while (s_writer_task) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    if (xSemaphoreTake(s_ring_mutex, portMAX_DELAY) == pdTRUE) {
        core2_audio_reset_ring_locked();
        xSemaphoreGive(s_ring_mutex);
    }
    i2s_channel_disable(s_tx_chan);
    i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
    if (s_ring_buf) {
        heap_caps_free(s_ring_buf);
        s_ring_buf = NULL;
        s_ring_capacity = 0;
    }
}

void core2_audio_set_sample_rate(uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0) return;
    if (s_sample_rate_hz == sample_rate_hz) return;
    if (!s_tx_chan) {
        s_sample_rate_hz = sample_rate_hz;
        return;
    }

    ESP_ERROR_CHECK(i2s_channel_disable(s_tx_chan));
    if (xSemaphoreTake(s_ring_mutex, portMAX_DELAY) == pdTRUE) {
        core2_audio_reset_ring_locked();
        xSemaphoreGive(s_ring_mutex);
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg));

    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    s_sample_rate_hz = sample_rate_hz;
}

static int16_t scale_sample(int16_t s, int volume_percent)
{
    if (volume_percent >= 100) return s;
    if (volume_percent <= 0) return 0;
    int32_t scaled = ((int32_t)s * (int32_t)volume_percent) / 100;
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    return (int16_t)scaled;
}

void core2_audio_write_pcm(const int16_t *samples,
                           size_t sample_count,
                           int channels,
                           int volume_percent)
{
    if (!s_tx_chan || !samples || sample_count == 0) return;
    if (channels != 1 && channels != 2) return;
    if (!s_ring_buf || s_ring_capacity == 0) return;
    if (!s_writer_running || !s_writer_task) return;

    int16_t out[CORE2_I2S_CHUNK_FRAMES * 2];
    size_t in_pos = 0;

    while (in_pos < sample_count) {
        size_t frames = CORE2_I2S_CHUNK_FRAMES;
        if (channels == 2) {
            size_t rem_frames = (sample_count - in_pos) / 2;
            if (rem_frames < frames) frames = rem_frames;
            if (frames == 0) break;
            for (size_t i = 0; i < frames; i++) {
                int16_t l = scale_sample(samples[in_pos + (i * 2)], volume_percent);
                int16_t r = scale_sample(samples[in_pos + (i * 2) + 1], volume_percent);
                out[i * 2] = l;
                out[i * 2 + 1] = r;
            }
            in_pos += frames * 2;
        } else {
            size_t rem_frames = sample_count - in_pos;
            if (rem_frames < frames) frames = rem_frames;
            for (size_t i = 0; i < frames; i++) {
                int16_t s = scale_sample(samples[in_pos + i], volume_percent);
                out[i * 2] = s;
                out[i * 2 + 1] = s;
            }
            in_pos += frames;
        }

        size_t chunk_bytes = frames * 2 * sizeof(int16_t);
        size_t queued = 0;

        while (queued < chunk_bytes) {
            size_t pushed = 0;
            if (xSemaphoreTake(s_ring_mutex, portMAX_DELAY) == pdTRUE) {
                pushed = core2_audio_ring_write((const uint8_t *)out + queued,
                                                chunk_bytes - queued);
                xSemaphoreGive(s_ring_mutex);
            }

            TaskHandle_t writer_task = s_writer_task;
            if (!s_writer_running || writer_task == NULL) {
                return;
            }

            if (pushed == 0) {
                xTaskNotifyGive(writer_task);
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            queued += pushed;
            xTaskNotifyGive(writer_task);
        }
    }
}
