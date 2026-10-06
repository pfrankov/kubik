#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*mic_capture_toggle_fn)(void *context, bool enabled);
typedef int (*mic_capture_read_fn)(void *context, void *buffer, size_t bytes, size_t *read, uint32_t timeout_ms);

typedef struct {
    void *context;
    mic_capture_toggle_fn rx;
    mic_capture_toggle_fn adc;
    mic_capture_read_fn read;
    volatile bool rx_enabled;
    volatile bool adc_enabled;
    volatile bool ready;
    volatile uint32_t reads;
    volatile uint32_t idle_reads;
} mic_capture_t;

void mic_capture_init(mic_capture_t *capture, void *context, mic_capture_toggle_fn rx,
                      mic_capture_toggle_fn adc, mic_capture_read_fn read,
                      bool rx_enabled, bool adc_enabled);
bool mic_capture_sync(mic_capture_t *capture, bool requested);
bool mic_capture_read(mic_capture_t *capture, bool permitted, void *buffer, size_t bytes, uint32_t timeout_ms);

void mic_capture_center(int16_t *buf, size_t samples, int32_t *dc);
