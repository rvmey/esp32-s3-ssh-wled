#include "atom_audio.h"

#include <math.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Hardware constants ──────────────────────────────────────────────────── */

#define I2S_PORT        I2S_NUM_1
#define SAMPLE_RATE     16000
#define I2S_BCK_PIN     19
#define I2S_LRCK_PIN    33
#define I2S_DOUT_PIN    22
#define I2S_CHANNELS    2

/* ── Beep synthesis constants ────────────────────────────────────────────── */

#define BEEP_DURATION_MS    300
#define BEEP_SAMPLES        ((SAMPLE_RATE * BEEP_DURATION_MS) / 1000)  /* 4800 */
#define BEEP_AMPLITUDE      16000   /* ~half full-scale — safe for speaker */

static const char *TAG = "atom_audio";

static i2s_chan_handle_t s_tx_chan = NULL;

/* ── I²S init ────────────────────────────────────────────────────────────── */

void atom_audio_init(void)
{
    if (s_tx_chan) return;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  /* silence DMA underflow */

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_PIN,
            .ws   = I2S_LRCK_PIN,
            .dout = I2S_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    ESP_LOGI(TAG, "I²S TX ready on BCK=%d LRCK=%d DOUT=%d @ %d Hz",
             I2S_BCK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, SAMPLE_RATE);
}

void atom_audio_deinit(void)
{
    if (!s_tx_chan) return;
    i2s_channel_disable(s_tx_chan);
    i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
    ESP_LOGI(TAG, "I²S TX uninstalled");
}

/* ── Clip playback ───────────────────────────────────────────────────────── */

void atom_audio_play_clip(const int16_t *samples, size_t num_samples,
                          uint32_t sample_rate)
{
    if (!s_tx_chan) {
        ESP_LOGW(TAG, "atom_audio_init() not called");
        return;
    }

    if (!samples || num_samples == 0) return;

    /* Chunk size: 256 mono samples -> 512 interleaved stereo samples */
#define CHUNK_MONO 256
    int16_t buf[CHUNK_MONO * I2S_CHANNELS];
    size_t  in_pos = 0;

    while (in_pos < num_samples) {
        size_t mono_in = num_samples - in_pos;
        if (sample_rate < SAMPLE_RATE) {
            /* 2x upsample path: each source sample becomes two output samples */
            if (mono_in > (CHUNK_MONO / 2)) mono_in = CHUNK_MONO / 2;
        } else {
            if (mono_in > CHUNK_MONO) mono_in = CHUNK_MONO;
        }

        size_t frames = 0;
        for (size_t i = 0; i < mono_in; i++) {
            int16_t s = samples[in_pos + i];
            if (sample_rate < SAMPLE_RATE) {
                /* Two frames per source sample for simple 2x upsample */
                buf[(frames * 2) + 0] = s;
                buf[(frames * 2) + 1] = s;
                frames++;
                buf[(frames * 2) + 0] = s;
                buf[(frames * 2) + 1] = s;
                frames++;
            } else {
                /* Duplicate mono sample to both left/right slots */
                buf[(frames * 2) + 0] = s;
                buf[(frames * 2) + 1] = s;
                frames++;
            }
        }
        in_pos += mono_in;

        size_t written = 0;
        i2s_channel_write(s_tx_chan, buf, frames * I2S_CHANNELS * sizeof(int16_t),
                          &written, portMAX_DELAY);
    }

    /* Short silence gap after each clip to prevent pops */
    int16_t silence[64] = {0};
    size_t  written = 0;
    i2s_channel_write(s_tx_chan, silence, sizeof(silence), &written, portMAX_DELAY);
#undef CHUNK_MONO
}

/* ── Tone synthesis ──────────────────────────────────────────────────────── */

static void play_tone(uint32_t freq_hz)
{
    if (!s_tx_chan) {
        ESP_LOGW(TAG, "atom_audio_init() not called");
        return;
    }

    float   step = 2.0f * (float)M_PI * (float)freq_hz / (float)SAMPLE_RATE;
#define TONE_CHUNK_FRAMES 256
    int16_t buf[TONE_CHUNK_FRAMES * I2S_CHANNELS];

    for (int base = 0; base < BEEP_SAMPLES; base += TONE_CHUNK_FRAMES) {
        int frames = BEEP_SAMPLES - base;
        if (frames > TONE_CHUNK_FRAMES) frames = TONE_CHUNK_FRAMES;

        for (int i = 0; i < frames; i++) {
            int idx = base + i;
            /* Apply simple linear fade-out over last 20% of tone to avoid click */
            float amp = BEEP_AMPLITUDE;
            if (idx > (int)(BEEP_SAMPLES * 0.8f)) {
                amp *= (float)(BEEP_SAMPLES - idx) / (float)(BEEP_SAMPLES * 0.2f);
            }
            int16_t s = (int16_t)(sinf((float)idx * step) * amp);
            buf[(i * 2) + 0] = s;
            buf[(i * 2) + 1] = s;
        }

        size_t written = 0;
        i2s_channel_write(s_tx_chan, buf, frames * I2S_CHANNELS * sizeof(int16_t),
                          &written, portMAX_DELAY);
    }
#undef TONE_CHUNK_FRAMES
}

void atom_audio_beep_ok(void)
{
    play_tone(440);   /* low A */
    play_tone(880);   /* high A — ascending */
}

void atom_audio_beep_fail(void)
{
    play_tone(880);   /* high A */
    play_tone(220);   /* low A — descending */
}
