#include "core2_mic.h"

#include <string.h>
#include <stdlib.h>
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"

/* ── Hardware constants ──────────────────────────────────────────────────── */

/*
 * I2S_NUM_0 — the speaker uses I2S_NUM_1; these can't share a port.
 * GPIO 0  — PDM CLK (shared with NS4168 WS on I2S_NUM_1; deinit speaker first).
 * GPIO 34 — PDM DATA (input-only GPIO, suitable for I2S DIN).
 * Slot LEFT — SPM1423 SEL=GND on Core2 → left channel.
 */
#define MIC_I2S_PORT    I2S_NUM_0
#define MIC_CLK_PIN     0
#define MIC_DATA_PIN    34
#define MIC_SAMPLE_RATE 16000
#define MIC_MAX_MS      4000   /* 4 s max — 128 KB PCM, allocated in PSRAM */

#define DMA_BUF_SAMPLES  512
#define DMA_BUF_BYTES    (DMA_BUF_SAMPLES * sizeof(int16_t))

static const char *TAG = "core2_mic";

static i2s_chan_handle_t s_rx_chan = NULL;

/* ── WAV header helper ───────────────────────────────────────────────────── */

static void write_wav_header(uint8_t *hdr, uint32_t pcm_bytes)
{
    uint32_t byte_rate  = MIC_SAMPLE_RATE * 1 * 2;
    uint32_t riff_size  = 36 + pcm_bytes;

    memcpy(hdr,      "RIFF", 4);
    memcpy(hdr + 4,  &riff_size, 4);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmt_size = 16;      memcpy(hdr + 16, &fmt_size, 4);
    uint16_t fmt_pcm  = 1;       memcpy(hdr + 20, &fmt_pcm, 2);
    uint16_t channels = 1;       memcpy(hdr + 22, &channels, 2);
    uint32_t sr = MIC_SAMPLE_RATE; memcpy(hdr + 24, &sr, 4);
                                 memcpy(hdr + 28, &byte_rate, 4);
    uint16_t align = 2;          memcpy(hdr + 32, &align, 2);
    uint16_t bits = 16;          memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &pcm_bytes, 4);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t core2_mic_init(void)
{
    if (s_rx_chan) return ESP_OK;

    /* Keep the DMA footprint small (3 x 256 = 1.5 KB vs the old 4 x 512 = 4 KB)
     * so the mic can be initialized even when BT + WiFi + TLS + the always-on
     * SIP tasks have fragmented internal DMA-capable SRAM. Recording/streaming
     * read in a loop, so a shallower DMA is fine. Larger configs caused
     * i2s_alloc_dma_desc to fail ("Voice: no audio captured"). */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 3;
    chan_cfg.dma_frame_num = 256;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        s_rx_chan = NULL;
        return err;
    }

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = MIC_SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
            /* DSR_16S: bclk = 16000 × 128 = 2.048 MHz — within SPM1423 spec.
             * Default DSR_8S (1.024 MHz) is at the absolute minimum and causes
             * the mic to output a constant DC value on some boards. */
            .dn_sample_mode = I2S_PDM_DSR_16S,
            .bclk_div       = 8,
        },
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk  = MIC_CLK_PIN,
            .din  = MIC_DATA_PIN,
            .invert_flags = { .clk_inv = false },
        },
    };
    /* SPM1423 SEL=GND on Core2 → data on left channel */
    pdm_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;
    err = i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_pdm_rx_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return err;
    }

    uint32_t clk_out_sig = GPIO.func_out_sel_cfg[MIC_CLK_PIN].func_sel;
    ESP_LOGI(TAG, "Diag: GPIO%d out_sel=%lu  I2S0I_WS=%d",
             MIC_CLK_PIN, (unsigned long)clk_out_sig, I2S0I_WS_OUT_IDX);
    ESP_LOGI(TAG, "PDM RX ready: CLK=%d DATA=%d @ %d Hz DSR_16S",
             MIC_CLK_PIN, MIC_DATA_PIN, MIC_SAMPLE_RATE);
    return ESP_OK;
}

size_t core2_mic_record(uint8_t **wav_out, uint32_t max_ms)
{
    *wav_out = NULL;
    if (!s_rx_chan) {
        ESP_LOGE(TAG, "core2_mic_init() not called");
        return 0;
    }

    if (max_ms > MIC_MAX_MS) max_ms = MIC_MAX_MS;

    /* Enable and let the mic stabilise, then flush the DMA ring. */
    i2s_channel_enable(s_rx_chan);
    vTaskDelay(pdMS_TO_TICKS(50));

    {
        int clk_hi = 0, data_hi = 0;
        const int NSAMP = 10000;
        for (int i = 0; i < NSAMP; i++) {
            clk_hi  += gpio_get_level(MIC_CLK_PIN);
            data_hi += gpio_get_level(MIC_DATA_PIN);
        }
        ESP_LOGI(TAG, "Diag: CLK GPIO%d %d/%d hi   DATA GPIO%d %d/%d hi",
                 MIC_CLK_PIN, clk_hi, NSAMP, MIC_DATA_PIN, data_hi, NSAMP);
    }

    {
        uint8_t flush[DMA_BUF_BYTES];
        size_t  flushed = 0;
        for (int i = 0; i < 4; i++) {
            i2s_channel_read(s_rx_chan, flush, DMA_BUF_BYTES, &flushed, pdMS_TO_TICKS(50));
        }
    }

    uint32_t max_seconds   = (max_ms + 999) / 1000;
    uint32_t max_pcm_bytes = (uint32_t)MIC_SAMPLE_RATE * 2 * max_seconds;

    /* Prefer PSRAM for the recording buffer — 4 s at 16 kHz = 128 KB. */
    uint8_t *pcm_buf = heap_caps_malloc(44 + max_pcm_bytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm_buf) {
        pcm_buf = malloc(44 + max_pcm_bytes);
    }
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate %lu B recording buffer",
                 (unsigned long)(44 + max_pcm_bytes));
        i2s_channel_disable(s_rx_chan);
        return 0;
    }

    uint8_t  *pcm_start  = pcm_buf + 44;
    uint32_t  pcm_written = 0;
    TickType_t start_tick = xTaskGetTickCount();

    while (1) {
        uint32_t elapsed = (uint32_t)((xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS);
        if (elapsed >= max_ms) break;
        if (pcm_written + DMA_BUF_BYTES > max_pcm_bytes) break;

        size_t   bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan,
                                         pcm_start + pcm_written,
                                         DMA_BUF_BYTES,
                                         &bytes_read,
                                         pdMS_TO_TICKS(50));
        if (err == ESP_OK) {
            pcm_written += (uint32_t)bytes_read;
        } else if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "i2s_channel_read: %s", esp_err_to_name(err));
            break;
        }
    }

    i2s_channel_disable(s_rx_chan);

    /* ── Post-processing ─────────────────────────────────────────────────── */
    {
        int16_t *samples = (int16_t *)pcm_start;
        uint32_t n = pcm_written / 2;

        if (n >= 8) {
            ESP_LOGI(TAG, "Raw[0..7]: %d %d %d %d %d %d %d %d",
                     samples[0], samples[1], samples[2], samples[3],
                     samples[4], samples[5], samples[6], samples[7]);
        }
        {
            int16_t first = (n > 0) ? samples[0] : 0;
            bool all_same = true;
            int32_t raw_min = 32767, raw_max = -32768;
            for (uint32_t i = 0; i < n; i++) {
                int32_t v = (int32_t)samples[i];
                if (v < raw_min) raw_min = v;
                if (v > raw_max) raw_max = v;
                if (samples[i] != first) all_same = false;
            }
            ESP_LOGI(TAG, "Raw: min=%ld max=%ld range=%ld %s",
                     (long)raw_min, (long)raw_max, (long)(raw_max - raw_min),
                     all_same ? "*** ALL IDENTICAL — mic stuck ***" : "ok");
        }

        /* IIR high-pass filter: y[n] = α·(y[n−1] + x[n] − x[n−1]), α≈0.9922 */
        int32_t hp_state = 0, prev_in = (n > 0) ? (int32_t)samples[0] : 0;
        for (uint32_t i = 0; i < n; i++) {
            int32_t x = (int32_t)samples[i];
            int32_t y = (127 * (hp_state + x - prev_in)) / 128;
            prev_in  = x;
            hp_state = y;
            if (y >  32767) y =  32767;
            if (y < -32768) y = -32768;
            samples[i] = (int16_t)y;
        }

        /* 8× software gain with saturation */
        int32_t peak = 0;
        for (uint32_t i = 0; i < n; i++) {
            int32_t s = (int32_t)samples[i] * 8;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            samples[i] = (int16_t)s;
            int32_t as = s < 0 ? -s : s;
            if (as > peak) peak = as;
        }
        ESP_LOGI(TAG, "Post-gain peak=%ld, %lu B WAV", (long)peak, (unsigned long)(44 + pcm_written));
    }

    if (pcm_written == 0) {
        free(pcm_buf);
        return 0;
    }

    write_wav_header(pcm_buf, pcm_written);
    *wav_out = pcm_buf;
    return 44 + (size_t)pcm_written;
}

/* ── Streaming capture (live audio for SIP) ──────────────────────────────── */

static int32_t s_stream_hp_state;
static int32_t s_stream_prev_in;
static bool    s_stream_dsp_init;

esp_err_t core2_mic_stream_start(void)
{
    if (!s_rx_chan) {
        ESP_LOGE(TAG, "core2_mic_init() not called");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "stream enable: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    /* Flush stale DMA frames so the stream starts fresh. */
    uint8_t flush[DMA_BUF_BYTES];
    size_t  flushed = 0;
    for (int i = 0; i < 4; i++) {
        i2s_channel_read(s_rx_chan, flush, DMA_BUF_BYTES, &flushed, pdMS_TO_TICKS(20));
    }
    s_stream_dsp_init = false;
    return ESP_OK;
}

size_t core2_mic_read_frame(int16_t *out, size_t max_samples)
{
    if (!s_rx_chan || !out || max_samples == 0) return 0;

    size_t want = max_samples * sizeof(int16_t);
    size_t got = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, out, want, &got, pdMS_TO_TICKS(60));
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "stream read: %s", esp_err_to_name(err));
        return 0;
    }
    uint32_t n = got / sizeof(int16_t);
    if (n == 0) return 0;

    if (!s_stream_dsp_init) {
        s_stream_prev_in = (int32_t)out[0];
        s_stream_hp_state = 0;
        s_stream_dsp_init = true;
    }
    /* Same DSP as core2_mic_record: IIR high-pass (α≈0.9922) then 8× gain. */
    for (uint32_t i = 0; i < n; i++) {
        int32_t x = (int32_t)out[i];
        int32_t y = (127 * (s_stream_hp_state + x - s_stream_prev_in)) / 128;
        s_stream_prev_in  = x;
        s_stream_hp_state = y;
        int32_t s = y * 8;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        out[i] = (int16_t)s;
    }
    return n;
}

void core2_mic_stream_stop(void)
{
    if (!s_rx_chan) return;
    esp_err_t err = i2s_channel_disable(s_rx_chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "stream disable: %s", esp_err_to_name(err));
    }
}

void core2_mic_deinit(void)
{
    if (!s_rx_chan) return;
    esp_err_t err = i2s_channel_disable(s_rx_chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "i2s_channel_disable: %s", esp_err_to_name(err));
    }
    i2s_del_channel(s_rx_chan);
    s_rx_chan = NULL;
    ESP_LOGI(TAG, "PDM RX uninstalled");
}
