#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Core2 external analog microphone driver via Grove Port B.
 *
 * Designed for a MAX4466 (or similar op-amp mic breakout) connected to:
 *   Grove Port B pin 2 (3.3 V) — MAX4466 VCC
 *   Grove Port B pin 1 (GND)  — MAX4466 GND
 *   Grove Port B pin 4 (GPIO 36 = ADC1_CH0) — MAX4466 OUT
 *
 * Uses the ADC continuous-mode driver (DMA-backed) so it does not
 * conflict with either I2S port.  Safe to call while WiFi is active.
 *
 * API surface is identical to core2_mic.h so callers can swap between
 * the built-in PDM mic and this driver without any other changes.
 */

esp_err_t core2_adc_mic_init(void);
size_t    core2_adc_mic_record(uint8_t **wav_out, uint32_t max_ms,
                              const volatile bool *stop_flag);
void      core2_adc_mic_deinit(void);
