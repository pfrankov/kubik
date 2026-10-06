#pragma once
#include <math.h>
#include "face.h"

static inline float frand(face_t *face) {
    uint32_t value = face->rng;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    face->rng = value;
    return (value & 0xFFFFFF) / (float)0x1000000;
}
static inline float frange(face_t *face, float low, float high) { return low + (high - low) * frand(face); }
static inline float clampf(float value, float low, float high) {
    return value < low ? low : value > high ? high : value;
}
static inline float smooth01(float value) {
    value = clampf(value, 0, 1);
    return value * value * (3 - 2 * value);
}
