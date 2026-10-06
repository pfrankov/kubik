#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*mic_task_request_fn)(void *context);
typedef bool (*mic_task_sync_fn)(void *context, bool requested);
typedef bool (*mic_task_read_fn)(void *context, bool permitted, void *buffer, size_t bytes, uint32_t timeout_ms);
typedef void (*mic_task_frame_fn)(void *context, int16_t *frame, const uint8_t *reference, int32_t *dc, uint32_t epoch);

typedef struct {
    void *context;
    mic_task_request_fn requested;
    mic_task_sync_fn sync;
    mic_task_read_fn read;
    mic_task_frame_fn deliver;
    uint32_t (*epoch)(void *context);
    bool (*duplex)(void *context);
    uint16_t slots;
    uint16_t chunk_samples;
    uint16_t frame_samples;
    uint16_t voice_slot;
    uint16_t timeout_ms;
} mic_task_config_t;

void mic_capture_task(void *arg);
