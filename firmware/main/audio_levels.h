#pragma once
#include <stdint.h>
#include <stdbool.h>
void audio_levels_set_speech(int percent);
void audio_levels_set_interface(int percent);
bool audio_levels_interface_enabled(void);
bool audio_levels_speech_enabled(void);
int16_t audio_levels_limit(int32_t value);
int16_t audio_levels_mix(int32_t value);
int16_t audio_levels_speech(int16_t sample);
int32_t audio_levels_interface(int32_t sample);
