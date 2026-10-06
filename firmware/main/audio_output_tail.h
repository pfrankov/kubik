#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "audio.h"

// One authoritative output geometry: six 10ms DMA descriptors and a 20ms mixed frame.
#define AUDIO_TX_DESCRIPTORS 6
#define AUDIO_TX_DESCRIPTOR_FRAMES 240
#define AUDIO_SPEAKER_FRAMES 480
#define AUDIO_OUTPUT_TAIL_US ((AUDIO_TX_DESCRIPTORS * AUDIO_TX_DESCRIPTOR_FRAMES + AUDIO_SPEAKER_FRAMES) * 1000000LL / AUDIO_RATE)
typedef struct { int64_t until; } audio_output_tail_t;
static inline void audio_output_tail_mark(audio_output_tail_t *tail, int64_t now) { tail->until = now + AUDIO_OUTPUT_TAIL_US; }
static inline bool audio_output_tail_ready(const audio_output_tail_t *tail, int64_t now) { return now >= tail->until; }
