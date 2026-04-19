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
#define MIC_CLK_PIN     0      /* PDM CLK  — ATOM Echo schematic: SPM1423 CLK = GPIO0  */
#define MIC_DATA_PIN    34     /* PDM DATA — ATOM Echo schematic: SPM1423 DATA = GPIO34 */
#define MIC_SAMPLE_RATE 16000
#define MIC_MAX_MS      2000   /* 2 s max — 64 KB PCM, fits in classic ESP32 fragmented heap */

/* DMA read chunk: 512 samples = 1 KB */
#define DMA_BUF_SAMPLES  512
#define DMA_BUF_BYTES    (DMA_BUF_SAMPLES * sizeof(int16_t))

static const char *TAG = "atom_mic";

static i2s_chan_handle_t s_rx_chan  = NULL;

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
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = DMA_BUF_SAMPLES;
    /* RX-only — pass NULL for tx handle */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = MIC_SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
            /* DSR_16S: bclk = 16000 × 128 = 2.048 MHz — comfortably within
             * SPM1423 spec (1.0–3.25 MHz).  The default macro uses DSR_8S
             * (1.024 MHz) which is at the absolute minimum and causes the mic
             * to output a constant DC value (all-zero PDM stream). */
            .dn_sample_mode = I2S_PDM_DSR_16S,
            .bclk_div       = 8,
        },
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk  = MIC_CLK_PIN,
            .din  = MIC_DATA_PIN,
            .invert_flags = { .clk_inv = false },
        },
    };
    /* SPM1423 SEL=VDD on ATOM Echo → data on rising CLK edge = right channel */
    pdm_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_RIGHT;
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm_cfg));
    /* Channel left disabled — enabled on-demand in atom_mic_record() */

    ESP_LOGI(TAG, "PDM RX ready: CLK=%d DATA=%d @ %d Hz DSR_16S",
             MIC_CLK_PIN, MIC_DATA_PIN, MIC_SAMPLE_RATE);
}

size_t atom_mic_record(uint8_t **wav_out, int button_gpio, uint32_t max_ms)
{
    if (!s_rx_chan) {
        ESP_LOGE(TAG, "atom_mic_init() not called");
        return 0;
    }

    if (max_ms > MIC_MAX_MS) max_ms = MIC_MAX_MS;

    /* Enable and flush stale DMA data.
     * Give the mic 50 ms to stabilise after the CLK starts before flushing,
     * then flush 4 buffers (4 × 32 ms = 128 ms) to clear the DMA ring. */
    i2s_channel_enable(s_rx_chan);
    vTaskDelay(pdMS_TO_TICKS(50));
    {
        uint8_t flush_buf[DMA_BUF_BYTES];
        size_t  flushed = 0;
        for (int fi = 0; fi < 4; fi++) {
            i2s_channel_read(s_rx_chan, flush_buf, DMA_BUF_BYTES,
                             &flushed, pdMS_TO_TICKS(50));
        }
    }

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
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read err=%s elapsed=%lu pcm=%lu", esp_err_to_name(err), (unsigned long)elapsed, (unsigned long)pcm_written);
            break;
        }
        pcm_written += (uint32_t)bytes_read;
    }

    /* Disable the channel so DMA ring cannot overflow during idle */
    i2s_channel_disable(s_rx_chan);

    /* Post-processing: IIR high-pass filter then software gain.
     *
     * The classic ESP32 PDM RX has no hardware HP filter — the large DC bias
     * (~−30000) comes through with the signal.  Mean subtraction was fragile
     * (one outlier sample drove the reported peak).  Instead we use a
     * single-pole IIR HPF:
     *
     *   y[n] = α·(y[n−1] + x[n] − x[n−1])
     *
     * with α = 127/128 ≈ 0.9922  →  fc = fs·(1−α)/(2π) ≈ 20 Hz
     *
     * This removes DC adaptively within the first ~50 ms of audio and passes
     * all speech frequencies (>300 Hz) with negligible attenuation.
     */
    {
        int16_t *samples = (int16_t *)pcm_start;
        uint32_t n = pcm_written / 2;

        /* Log first 8 raw samples and 8 samples from the middle to check
         * whether the DMA flush cleared the stale startup pattern and whether
         * there is any modulation (speech) in the middle of the recording. */
        if (n >= 8) {
            ESP_LOGI(TAG, "Raw[0..7]:   %d %d %d %d %d %d %d %d",
                     samples[0], samples[1], samples[2], samples[3],
                     samples[4], samples[5], samples[6], samples[7]);
        }
        uint32_t mid = n / 2;
        if (mid + 8 <= n) {
            ESP_LOGI(TAG, "Raw[mid..]: %d %d %d %d %d %d %d %d",
                     samples[mid+0], samples[mid+1], samples[mid+2], samples[mid+3],
                     samples[mid+4], samples[mid+5], samples[mid+6], samples[mid+7]);
        }

        /* Step 1 — IIR high-pass filter (removes DC + low-frequency bias) */
        int32_t hp_state = 0;
        int32_t prev_in  = (n > 0) ? (int32_t)samples[0] : 0;
        int32_t peak_hp  = 0;
        for (uint32_t i = 0; i < n; i++) {
            int32_t x = (int32_t)samples[i];
            int32_t y = (127 * (hp_state + x - prev_in)) / 128;
            prev_in  = x;
            hp_state = y;
            int32_t ay = y < 0 ? -y : y;
            if (ay > peak_hp) peak_hp = ay;
            if (y >  32767) y =  32767;
            if (y < -32768) y = -32768;
            samples[i] = (int16_t)y;
        }
        ESP_LOGI(TAG, "Post-HPF: peak=%ld n=%lu", (long)peak_hp, (unsigned long)n);

        /* Step 2 — 8x software gain with saturation */
        int32_t peak_final = 0;
        for (uint32_t i = 0; i < n; i++) {
            int32_t s = (int32_t)samples[i] * 8;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            samples[i] = (int16_t)s;
            int32_t as = s < 0 ? -s : s;
            if (as > peak_final) peak_final = as;
        }
        ESP_LOGI(TAG, "Post-gain: peak=%ld", (long)peak_final);
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
