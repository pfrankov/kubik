#pragma once
#include <stdbool.h>
#include <stdint.h>

// The speaker's bounded ADPCM ring and decoder have one owner module.
void audio_ring_init(void);
bool audio_ring_read(int16_t *samples, int32_t *mix);
void audio_ring_release_idle(void);
bool audio_ring_active(void);
bool audio_ring_output_idle(void);
void audio_ring_output_written(void);
uint32_t audio_ring_epoch(void);
void audio_ring_note_mix(uint32_t epoch, uint32_t us);
void audio_ring_note_write(uint32_t epoch, uint32_t us);
