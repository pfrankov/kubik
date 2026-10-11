#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "audio_output_tail.h"

// All output, including local sounds. A feed gap above the DMA reserve is a
// starvation risk, not a measurement of the analog speaker or provider gaps.
typedef struct {
    uint32_t frames, audible_frames, mix_max_us, feed_max_us, late_feeds, write_errors;
    int64_t last_feed_us;
    bool was_audible;
} audio_output_profile_t;
static inline void audio_output_profile_gap(audio_output_profile_t *p, int64_t fed) {
    uint32_t gap = p->last_feed_us && fed - p->last_feed_us < UINT32_MAX ? (uint32_t)(fed - p->last_feed_us) : 0;
    if (gap > p->feed_max_us) p->feed_max_us = gap;
    if (gap > AUDIO_TX_DESCRIPTORS * AUDIO_TX_DESCRIPTOR_FRAMES * 1000000 / AUDIO_RATE && p->late_feeds < UINT32_MAX)
        ++p->late_feeds;
}
static inline void audio_output_profile_record(audio_output_profile_t *p, int64_t fed,
                                                uint32_t mix_us, bool audible, bool failed) {
    if (failed) { if (p->write_errors < UINT32_MAX) ++p->write_errors; return; }
    if (p->frames < UINT32_MAX) ++p->frames;
    if (audible && p->audible_frames < UINT32_MAX) ++p->audible_frames;
    if (audible || p->was_audible) {
        if (p->was_audible) audio_output_profile_gap(p, fed);
        if (mix_us > p->mix_max_us) p->mix_max_us = mix_us;
    }
    p->last_feed_us = fed;
    p->was_audible = audible;
}
audio_output_profile_t audio_output_profile_read(bool reset);
void audio_output_profile_note(int64_t fed, uint32_t mix_us, bool audible, bool failed);
