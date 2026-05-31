#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CORE2_LED_COUNT  10
#define CORE2_LED_GPIO   25

void  core2_leds_init(void);
bool  core2_leds_initialized(void);
void  core2_leds_set_solid(uint8_t r, uint8_t g, uint8_t b);
void  core2_leds_set_bands(const float *levels, int count);
void  core2_leds_off(void);

#ifdef __cplusplus
}
#endif
