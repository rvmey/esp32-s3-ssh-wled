#include "core2_adc_mic.h"

#include <string.h>
#include <stdlib.h>
#include "esp_adc/adc_continuous.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Hardware constants ──────────────────────────────────────────────────── */

/*
 * Grove Port B, pin 4 = GPIO 36 = ADC1 channel 0.
 * GPIO 36 is input-only and ADC1-capable — safe to use while WiFi is active.
 *
 * MAX4466 output is biased at VCC/2 ≈ 1.65 V with 3.3 V supply.
 * ADC_ATTEN_DB_12 measures 0–~3.3 V, so the DC midpoint maps to ~2048 in
 * the 12-bit range.  The IIR high-pass filter removes any residual DC offset.
 */
#define ADC_MIC_CHANNEL     ADC_CHANNEL_0      /* GPIO 36 */
#define ADC_MIC_UNIT        ADC_UNIT_1
#define ADC_MIC_ATTEN       ADC_ATTEN_DB_12    /* 0–~3.3 V input range */
#define ADC_MIC_BITWIDTH    ADC_BITWIDTH_12

#define ADC_MIC_SAMPLE_RATE 16000
#define ADC_MIC_MAX_MS      4000   /* 4 s max — 128 KB PCM, allocated in PSRAM */

/* ADC continuous-mode frame and store sizes.
 * frame_size must be a multiple of SOC_ADC_DIGI_DATA_BYTES_PER_CONV (4).
 * store_size must be >= frame_size * 2. */
#define ADC_MIC_FRAME_BYTES  256   /* 64 samples × 4 bytes each */
#define ADC_MIC_STORE_BYTES 1024

/* Each ADC continuous-mode result is 4 bytes (adc_digi_output_data_t).
 * Classic ESP32 with ADC1 uses TYPE1 output format. */
#define ADC_BYTES_PER_SAMPLE  sizeof(adc_digi_output_data_t)

/* DC midpoint: MAX4466 at 3.3 V supply → VCC/2 / (3.3 / 4096) ≈ 2048 */
#define ADC_MIC_DC_OFFSET  2048

/* Software gain after scaling to 16-bit (matches core2_mic.c behaviour) */
#define ADC_MIC_SW_GAIN  8

static const char *TAG = "core2_adc_mic";

static adc_continuous_handle_t s_adc_handle = NULL;

/* ── WAV header helper (identical to core2_mic.c) ────────────────────────── */

static void write_wav_header(uint8_t *hdr, uint32_t pcm_bytes)
{
    uint32_t byte_rate  = ADC_MIC_SAMPLE_RATE * 1 * 2;
    uint32_t riff_size  = 36 + pcm_bytes;

    memcpy(hdr,      "RIFF", 4);
    memcpy(hdr + 4,  &riff_size, 4);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmt_size = 16;      memcpy(hdr + 16, &fmt_size, 4);
    uint16_t fmt_pcm  = 1;       memcpy(hdr + 20, &fmt_pcm, 2);
    uint16_t channels = 1;       memcpy(hdr + 22, &channels, 2);
    uint32_t sr = ADC_MIC_SAMPLE_RATE; memcpy(hdr + 24, &sr, 4);
                                 memcpy(hdr + 28, &byte_rate, 4);
    uint16_t align = 2;          memcpy(hdr + 32, &align, 2);
    uint16_t bits  = 16;         memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &pcm_bytes, 4);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t core2_adc_mic_init(void)
{
    if (s_adc_handle) return ESP_OK;

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = ADC_MIC_STORE_BYTES,
        .conv_frame_size    = ADC_MIC_FRAME_BYTES,
    };
    esp_err_t err = adc_continuous_new_handle(&handle_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_new_handle: %s", esp_err_to_name(err));
        return err;
    }

    adc_digi_pattern_config_t pattern = {
        .atten     = ADC_MIC_ATTEN,
        .channel   = ADC_MIC_CHANNEL,
        .unit      = ADC_MIC_UNIT,
        .bit_width = ADC_MIC_BITWIDTH,
    };
    adc_continuous_config_t dig_cfg = {
        .pattern_num    = 1,
        .adc_pattern    = &pattern,
        .sample_freq_hz = ADC_MIC_SAMPLE_RATE,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    err = adc_continuous_config(s_adc_handle, &dig_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_config: %s", esp_err_to_name(err));
        adc_continuous_deinit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }

    ESP_LOGI(TAG, "ADC mic ready: GPIO36 (ADC1_CH0) @ %d Hz, atten=12dB",
             ADC_MIC_SAMPLE_RATE);
    return ESP_OK;
}

size_t core2_adc_mic_record(uint8_t **wav_out, uint32_t max_ms)
{
    *wav_out = NULL;
    if (!s_adc_handle) {
        ESP_LOGE(TAG, "core2_adc_mic_init() not called");
        return 0;
    }

    if (max_ms > ADC_MIC_MAX_MS) max_ms = ADC_MIC_MAX_MS;

    esp_err_t err = adc_continuous_start(s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_start: %s", esp_err_to_name(err));
        return 0;
    }
    vTaskDelay(pdMS_TO_TICKS(50));   /* let MAX4466 bias stabilise */

    uint32_t max_samples   = (uint32_t)ADC_MIC_SAMPLE_RATE * ((max_ms + 999) / 1000);
    uint32_t max_pcm_bytes = max_samples * 2;

    /* Prefer PSRAM for the recording buffer — 4 s at 16 kHz = 128 KB */
    uint8_t *pcm_buf = heap_caps_malloc(44 + max_pcm_bytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm_buf) {
        pcm_buf = malloc(44 + max_pcm_bytes);
    }
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate %lu B recording buffer",
                 (unsigned long)(44 + max_pcm_bytes));
        adc_continuous_stop(s_adc_handle);
        return 0;
    }

    int16_t *pcm_samples  = (int16_t *)(pcm_buf + 44);
    uint32_t sample_count = 0;

    /* Scratch buffer for one DMA frame of raw ADC output words */
    uint8_t raw[ADC_MIC_FRAME_BYTES];

    TickType_t start_tick = xTaskGetTickCount();

    while (1) {
        uint32_t elapsed = (uint32_t)((xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS);
        if (elapsed >= max_ms) break;
        if (sample_count >= max_samples) break;

        uint32_t out_len = 0;
        err = adc_continuous_read(s_adc_handle, raw, ADC_MIC_FRAME_BYTES,
                                  &out_len, pdMS_TO_TICKS(50));
        if (err == ESP_ERR_TIMEOUT) continue;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc_continuous_read: %s", esp_err_to_name(err));
            break;
        }

        uint32_t n_entries = out_len / (uint32_t)ADC_BYTES_PER_SAMPLE;
        const adc_digi_output_data_t *entries = (const adc_digi_output_data_t *)raw;

        for (uint32_t i = 0; i < n_entries && sample_count < max_samples; i++) {
            /* Filter out stray samples from other channels (shouldn't happen
             * with a single-pattern config, but guard defensively). */
            if (entries[i].type1.channel != (uint32_t)ADC_MIC_CHANNEL) continue;

            /* 12-bit ADC value (0–4095) → signed 16-bit centred on 0.
             * Subtract the DC midpoint then scale to 16-bit range. */
            int32_t v = ((int32_t)entries[i].type1.data - ADC_MIC_DC_OFFSET) * 16;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            pcm_samples[sample_count++] = (int16_t)v;
        }
    }

    adc_continuous_stop(s_adc_handle);

    /* ── Post-processing (same as core2_mic.c) ───────────────────────────── */
    {
        uint32_t n = sample_count;

        if (n >= 8) {
            ESP_LOGI(TAG, "Raw[0..7]: %d %d %d %d %d %d %d %d",
                     pcm_samples[0], pcm_samples[1], pcm_samples[2], pcm_samples[3],
                     pcm_samples[4], pcm_samples[5], pcm_samples[6], pcm_samples[7]);
        }
        {
            int32_t raw_min = 32767, raw_max = -32768;
            for (uint32_t i = 0; i < n; i++) {
                int32_t v = (int32_t)pcm_samples[i];
                if (v < raw_min) raw_min = v;
                if (v > raw_max) raw_max = v;
            }
            ESP_LOGI(TAG, "Raw: min=%ld max=%ld range=%ld",
                     (long)raw_min, (long)raw_max, (long)(raw_max - raw_min));
        }

        /* IIR high-pass filter: removes DC offset and low-frequency rumble.
         * y[n] = α·(y[n−1] + x[n] − x[n−1]), α ≈ 0.9922 (same as PDM driver) */
        int32_t hp_state = 0, prev_in = (n > 0) ? (int32_t)pcm_samples[0] : 0;
        for (uint32_t i = 0; i < n; i++) {
            int32_t x = (int32_t)pcm_samples[i];
            int32_t y = (127 * (hp_state + x - prev_in)) / 128;
            prev_in  = x;
            hp_state = y;
            if (y >  32767) y =  32767;
            if (y < -32768) y = -32768;
            pcm_samples[i] = (int16_t)y;
        }

        /* Software gain with saturation */
        int32_t peak = 0;
        for (uint32_t i = 0; i < n; i++) {
            int32_t s = (int32_t)pcm_samples[i] * ADC_MIC_SW_GAIN;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            pcm_samples[i] = (int16_t)s;
            int32_t as = s < 0 ? -s : s;
            if (as > peak) peak = as;
        }
        ESP_LOGI(TAG, "Post-gain peak=%ld, %lu samples, %lu B WAV",
                 (long)peak, (unsigned long)sample_count,
                 (unsigned long)(44 + sample_count * 2));
    }

    if (sample_count == 0) {
        free(pcm_buf);
        return 0;
    }

    uint32_t pcm_bytes = sample_count * 2;
    write_wav_header(pcm_buf, pcm_bytes);
    *wav_out = pcm_buf;
    return 44 + (size_t)pcm_bytes;
}

void core2_adc_mic_deinit(void)
{
    if (!s_adc_handle) return;
    adc_continuous_deinit(s_adc_handle);
    s_adc_handle = NULL;
    ESP_LOGI(TAG, "ADC mic uninstalled");
}
