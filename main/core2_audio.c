#include "core2_audio.h"

#include <string.h>
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define CORE2_I2S_PORT      I2S_NUM_1
#define CORE2_I2S_BCLK_GPIO 12
#define CORE2_I2S_WS_GPIO   0
#define CORE2_I2S_DOUT_GPIO 2
#define CORE2_I2S_CHUNK_FRAMES 256

static const char *TAG = "core2_audio";

static i2s_chan_handle_t s_tx_chan = NULL;
static uint32_t s_sample_rate_hz = 44100;

void core2_audio_init(void)
{
    if (s_tx_chan) return;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CORE2_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

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

    ESP_LOGI(TAG, "I2S TX ready on BCK=%d WS=%d DOUT=%d @ %lu Hz",
             CORE2_I2S_BCLK_GPIO,
             CORE2_I2S_WS_GPIO,
             CORE2_I2S_DOUT_GPIO,
             (unsigned long)s_sample_rate_hz);
}

void core2_audio_deinit(void)
{
    if (!s_tx_chan) return;
    i2s_channel_disable(s_tx_chan);
    i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
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

        size_t written = 0;
        i2s_channel_write(s_tx_chan,
                          out,
                          frames * 2 * sizeof(int16_t),
                          &written,
                          portMAX_DELAY);
    }
}
