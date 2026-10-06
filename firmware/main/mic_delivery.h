#pragma once

#include "mic_capture_task.h"

// Active recording only: a bounded 1200 ms queue separates DMA reads from TLS/VAD.
// NULL frame reports overflow once; callers must abort that turn, not accept gaps.
typedef void (*mic_delivery_frame_fn)(void *context, const int16_t *pcm, const uint8_t *ima, uint32_t epoch);

bool mic_delivery_start(mic_delivery_frame_fn deliver, void *context, bool duplex);
void mic_delivery_push(const int16_t *frame, uint32_t epoch, const uint8_t *reference);
// Capture must be stopped first. Drains accepted frames and joins before returning.
void mic_delivery_stop(void);
