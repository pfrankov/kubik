#include "audio_signal.h"
#include <math.h>

float audio_signal_level(const int16_t *p, int n, float floor_db, float range_db) {
    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += (int32_t)p[i] * p[i];
    float rms = sqrtf((float)sum / n) + 1.f;
    float db = 20.f * log10f(rms / 32768.f);
    float l = (db - floor_db) / range_db;
    return l < 0 ? 0 : l > 1 ? 1 : l;
}
