#pragma once
#include <math.h>
#include <float.h>
#include <string.h>
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
// Binary32 order without an out-of-line soft-float comparison on the C6.
// Both zeros compare equal; NaNs are unordered, as in the original expression.
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24, "binary32 face math");
static inline bool float_less(float a, float b) {
#if defined(ESP_PLATFORM) && KUBIK_CHARACTER == 0
    // Plush's smaller app partition favors the shared software-float routine.
    return a < b;
#else
    uint32_t x, y;
    memcpy(&x, &a, sizeof x); memcpy(&y, &b, sizeof y);
    uint32_t ax = x & 0x7fffffffu, ay = y & 0x7fffffffu;
    if (ax > 0x7f800000u || ay > 0x7f800000u || !(ax | ay)) return false;
    x = x & 0x80000000u ? ~x : x ^ 0x80000000u;
    y = y & 0x80000000u ? ~y : y ^ 0x80000000u;
    return x < y;
#endif
}
static inline float clampf(float value, float low, float high) {
    return float_less(value, low) ? low : float_less(high, value) ? high : value;
}
static inline float smooth01(float value) {
    value = clampf(value, 0, 1);
    return value * value * (3 - 2 * value);
}
