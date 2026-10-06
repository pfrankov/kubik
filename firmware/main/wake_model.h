// Tessa streaming model. Caller serializes feed/start/stop with capture teardown.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool wake_model_start(void);
bool wake_model_healthy(void);
void wake_model_stop(void);
bool wake_model_feed(const int16_t *pcm, size_t samples);
uint32_t wake_model_max_us(void);
uint8_t wake_model_probability(void);
uint8_t wake_model_peak_probability(void);
uint32_t wake_model_average_us(void);
#ifdef __cplusplus
}
#endif
