#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "character.h"
void audio_sfx_init(void);
void audio_sfx_note_output(bool audible);
void audio_sfx_quiet(void);
void audio_sfx_mix(int32_t *accumulator, int samples);
