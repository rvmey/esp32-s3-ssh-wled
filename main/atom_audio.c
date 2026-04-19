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
/* GPIO33 is shared with mic PDM CLK; don't use it for speaker LRCK */
#define I2S_LRCK_PIN    I2S_GPIO_UNUSED  /* NS4168 mono mode — BCK timing sufficient */
#define I2S_DOUT_PIN    22

/* ── Beep synthesis constants ────────────────────────────────────────────── */

#define BEEP_DURATION_MS    300
#define BEEP_SAMPLES        ((SAMPLE_RATE * BEEP_DURATION_MS) / 1000)  /* 4800 */
#define BEEP_AMPLITUDE      16000   /* ~half full-scale — safe for speaker */

static const char *TAG = "atom_audio";

static i2s_chan_handle_t s_tx_chan = NULL;

/* ── I²S init ────────────────────────────────────────────────────────────── */

void atom_audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  /* silence DMA underflow */

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_MONO),
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

    ESP_LOGI(TAG, "I²S TX ready on BCK=%d DOUT=%d (WS unused) @ %d Hz",
             I2S_BCK_PIN, I2S_DOUT_PIN, SAMPLE_RATE);
}

/* ── Clip playback ───────────────────────────────────────────────────────── */

void atom_audio_play_clip(const int16_t *samples, size_t num_samples,
                          uint32_t sample_rate)
{
    if (!samples || num_samples == 0) return;

    /* Chunk size: 512 samples at a time to keep stack usage low */
#define CHUNK 512
    int16_t buf[CHUNK];
    size_t  in_pos = 0;

    while (in_pos < num_samples) {
        size_t chunk_out = 0;

        if (sample_rate < SAMPLE_RATE) {
            /* Simple 2× upsample: duplicate each source sample */
            while (chunk_out + 1 < CHUNK && in_pos < num_samples) {
                buf[chunk_out++] = samples[in_pos];
                buf[chunk_out++] = samples[in_pos];
                in_pos++;
            }
        } else {
            /* No upsampling needed */
            size_t copy = num_samples - in_pos;
            if (copy > CHUNK) copy = CHUNK;
            memcpy(buf, samples + in_pos, copy * sizeof(int16_t));
            in_pos    += copy;
            chunk_out  = copy;
        }

        size_t written = 0;
        i2s_channel_write(s_tx_chan, buf, chunk_out * sizeof(int16_t),
                          &written, portMAX_DELAY);
    }

    /* Short silence gap after each clip to prevent pops */
    int16_t silence[64] = {0};
    size_t  written = 0;
    i2s_channel_write(s_tx_chan, silence, sizeof(silence), &written, portMAX_DELAY);
#undef CHUNK
}

/* ── Tone synthesis ──────────────────────────────────────────────────────── */

static void play_tone(uint32_t freq_hz)
{
    int16_t buf[BEEP_SAMPLES];
    float   step = 2.0f * (float)M_PI * (float)freq_hz / (float)SAMPLE_RATE;

    for (int i = 0; i < BEEP_SAMPLES; i++) {
        /* Apply simple linear fade-out over last 20% of tone to avoid click */
        float amp = BEEP_AMPLITUDE;
        if (i > (int)(BEEP_SAMPLES * 0.8f)) {
            amp *= (float)(BEEP_SAMPLES - i) / (float)(BEEP_SAMPLES * 0.2f);
        }
        buf[i] = (int16_t)(sinf((float)i * step) * amp);
    }

    size_t written = 0;
    i2s_channel_write(s_tx_chan, buf, sizeof(buf), &written, portMAX_DELAY);
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
