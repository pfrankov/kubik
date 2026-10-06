#pragma once
#include <stdint.h>
float audio_signal_level(const int16_t *pcm, int samples, float floor_db, float range_db);
