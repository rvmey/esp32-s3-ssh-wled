#include "atom_mic.h"

#include <string.h>
#include <stdlib.h>
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Hardware constants ──────────────────────────────────────────────────── */

#define MIC_I2S_PORT    I2S_NUM_0
#define MIC_CLK_PIN     19     /* PDM CLK  — ATOM Echo schematic: BCLK/GPIO19 */
#define MIC_DATA_PIN    23     /* PDM DATA — ATOM Echo schematic: DATA_IN/GPIO23 */
#define MIC_SAMPLE_RATE 16000
#define MIC_MAX_MS      2000   /* 2 s max — 64 KB PCM, fits in classic ESP32 fragmented heap */

/* DMA read chunk: 512 samples = 1 KB */
#define DMA_BUF_SAMPLES  512
#define DMA_BUF_BYTES    (DMA_BUF_SAMPLES * sizeof(int16_t))

static const char *TAG = "atom_mic";

static i2s_chan_handle_t s_rx_chan = NULL;

/* ── WAV header helper ───────────────────────────────────────────────────── */

static void write_wav_header(uint8_t *hdr, uint32_t pcm_bytes)
{
    uint32_t byte_rate   = MIC_SAMPLE_RATE * 1 * 2;  /* SR × channels × bytes/sample */
    uint32_t data_chunk  = pcm_bytes;
    uint32_t riff_size   = 36 + data_chunk;

    /* RIFF chunk */
    memcpy(hdr,      "RIFF", 4);
    memcpy(hdr + 4,  &riff_size, 4);
    memcpy(hdr + 8,  "WAVE", 4);
    /* fmt  sub-chunk */
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmt_size = 16;           memcpy(hdr + 16, &fmt_size, 4);
    uint16_t audio_fmt = 1;           memcpy(hdr + 20, &audio_fmt, 2);  /* PCM */
    uint16_t channels  = 1;           memcpy(hdr + 22, &channels, 2);
    uint32_t sr = MIC_SAMPLE_RATE;    memcpy(hdr + 24, &sr, 4);
                                      memcpy(hdr + 28, &byte_rate, 4);
    uint16_t block_align = 2;         memcpy(hdr + 32, &block_align, 2);
    uint16_t bits = 16;               memcpy(hdr + 34, &bits, 2);
    /* data sub-chunk */
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_chunk, 4);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void atom_mic_init(void)
{
    if (s_rx_chan) return;  /* already installed */

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    /* Match DMA descriptor size to our read size so each i2s_channel_read()
     * drains exactly one descriptor — eliminates partial-read ambiguity.
     * 8 descriptors × 1024 bytes = 8 KB ring = 256 ms at 32 KB/s. */
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = DMA_BUF_SAMPLES;  /* 512 frames × 2 bytes = 1024 bytes/desc */
    /* RX only — pass NULL for tx_handle */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk  = MIC_CLK_PIN,
            .din  = MIC_DATA_PIN,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm_cfg));
    /* Channel is NOT enabled here — enabled on-demand in atom_mic_record()
     * to avoid DMA ring buffer overflow during idle periods. */

    ESP_LOGI(TAG, "PDM RX ready: CLK=%d DATA=%d @ %d Hz",
             MIC_CLK_PIN, MIC_DATA_PIN, MIC_SAMPLE_RATE);
}

size_t atom_mic_record(uint8_t **wav_out, int button_gpio, uint32_t max_ms)
{
    if (!s_rx_chan) {
        ESP_LOGE(TAG, "atom_mic_init() not called");
        return 0;
    }

    if (max_ms > MIC_MAX_MS) max_ms = MIC_MAX_MS;

    /* Enable the channel now (left disabled at init to avoid idle overflow).
     * No warmup delay — starting to read immediately keeps the ring from
     * filling up and stalling the DMA.  Any initial PDM-filter transient
     * (a few ms) is harmless for speech STT. */
    i2s_channel_enable(s_rx_chan);

    /* Calculate maximum PCM bytes and allocate buffer including WAV header.
     * Use ceiling division so e.g. 2000 ms → 2 s (not 3). */
    uint32_t max_seconds   = (max_ms + 999) / 1000;
    uint32_t max_pcm_bytes = (uint32_t)MIC_SAMPLE_RATE * 2 * max_seconds;
    uint8_t *pcm_buf = malloc(44 + max_pcm_bytes);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate %lu byte recording buffer",
                 (unsigned long)(44 + max_pcm_bytes));
        return 0;
    }

    uint8_t *pcm_start  = pcm_buf + 44;  /* WAV header lives in first 44 bytes */
    uint32_t pcm_written = 0;
    uint32_t deadline_ms = max_ms;
#define MIC_MIN_MS  300   /* minimum recording — OpenAI requires ≥ 0.1 s; use 300 ms for safety */
    TickType_t start_tick = xTaskGetTickCount();

    while (1) {
        TickType_t elapsed = (xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS;
        int btn_level = gpio_get_level(button_gpio);

        /* Stop if button released — but only after minimum duration to satisfy STT API */
        if (btn_level == 1 && elapsed >= MIC_MIN_MS) {
            ESP_LOGI(TAG, "Stop: btn released, elapsed=%lu ms, pcm=%lu", (unsigned long)elapsed, (unsigned long)pcm_written);
            break;
        }

        /* Stop if max duration reached */
        if (elapsed >= deadline_ms) {
            ESP_LOGI(TAG, "Stop: deadline, elapsed=%lu ms", (unsigned long)elapsed);
            break;
        }

        /* Stop if no more room in the buffer */
        if (pcm_written + DMA_BUF_BYTES > max_pcm_bytes) {
            ESP_LOGI(TAG, "Stop: buffer full, pcm=%lu", (unsigned long)pcm_written);
            break;
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan,
                                         pcm_start + pcm_written,
                                         DMA_BUF_BYTES,
                                         &bytes_read,
                                         pdMS_TO_TICKS(50));
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGD(TAG, "i2s timeout, elapsed=%lu btn=%d pcm=%lu", (unsigned long)elapsed, btn_level, (unsigned long)pcm_written);
            continue;  /* no DMA data yet — keep looping until button released */
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read err=%s elapsed=%lu pcm=%lu", esp_err_to_name(err), (unsigned long)elapsed, (unsigned long)pcm_written);
            break;
        }
        pcm_written += (uint32_t)bytes_read;
    }

    /* Disable the channel so the DMA ring cannot overflow during idle time */
    i2s_channel_disable(s_rx_chan);

    /* Diagnostic: log pre-gain peak and RMS so we can tune the gain factor.
     * Also log the first 8 raw sample values to verify PDM-to-PCM is working. */
    {
        int16_t *samples = (int16_t *)pcm_start;
        uint32_t n = pcm_written / 2;
        int32_t peak = 0;
        int64_t sum_sq = 0;
        for (uint32_t i = 0; i < n; i++) {
            int32_t v = samples[i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
            sum_sq += (int64_t)samples[i] * samples[i];
        }
        uint32_t rms = (n > 0) ? (uint32_t)( (int64_t)1 * (int64_t)( (sum_sq / n > 0) ? 1 : 0 ) ) : 0;
        /* integer sqrt approximation */
        if (n > 0) {
            uint64_t mean_sq = (uint64_t)(sum_sq / n);
            uint64_t r = mean_sq;
            /* Newton's method, a few iterations */
            if (r > 0) {
                uint64_t x = r;
                x = (x + mean_sq / x) / 2;
                x = (x + mean_sq / x) / 2;
                x = (x + mean_sq / x) / 2;
                x = (x + mean_sq / x) / 2;
                rms = (uint32_t)x;
            }
        }
        ESP_LOGI(TAG, "Pre-gain: peak=%ld rms=%lu n=%lu",
                 (long)peak, (unsigned long)rms, (unsigned long)n);
        if (n >= 8) {
            ESP_LOGI(TAG, "First 8 raw samples: %d %d %d %d %d %d %d %d",
                     samples[0], samples[1], samples[2], samples[3],
                     samples[4], samples[5], samples[6], samples[7]);
        }
    }

    /* Software gain: SPM1423 on classic ESP32 has no hardware amplify register.
     * Scale each sample by 8x with saturation to bring quiet mic up to normal. */
    {
        int16_t *samples = (int16_t *)pcm_start;
        uint32_t n = pcm_written / 2;
        for (uint32_t i = 0; i < n; i++) {
            int32_t s = (int32_t)samples[i] * 8;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            samples[i] = (int16_t)s;
        }
    }

    if (pcm_written == 0) {
        free(pcm_buf);
        *wav_out = NULL;
        return 0;
    }

    /* Write WAV header into the reserved first 44 bytes */
    write_wav_header(pcm_buf, pcm_written);

    *wav_out = pcm_buf;
    size_t total = 44 + (size_t)pcm_written;
    ESP_LOGI(TAG, "Recorded %lu bytes PCM → %zu byte WAV", (unsigned long)pcm_written, total);
    return total;
}

void atom_mic_deinit(void)
{
    if (!s_rx_chan) return;
    i2s_channel_disable(s_rx_chan);
    i2s_del_channel(s_rx_chan);
    s_rx_chan = NULL;
    ESP_LOGI(TAG, "PDM RX uninstalled");
}
