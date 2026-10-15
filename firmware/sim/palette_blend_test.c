#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../main/palette_blend.h"

static float from_bits(uint32_t bits) { float value; memcpy(&value, &bits, sizeof value); return value; }
static uint32_t reference(uint32_t a, uint32_t b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int channel = (a >> shift) & 255, target = (b >> shift) & 255;
        out |= (uint32_t)(channel + (int)((target - channel) * t)) << shift;
    }
    return out;
}
static void all_mantissas(void) {
    for (unsigned exponent = 119; exponent <= 126; exponent++)
        for (uint32_t mantissa = 0x800000; mantissa < 0x1000000; mantissa++) {
            float weight = from_bits((exponent << 23) | (mantissa & 0x7fffff));
            assert(palette_delta(255, mantissa, exponent) == (int)(255.f * weight));
        }
}
static void integer_boundaries(void) {
    for (int delta = 1; delta <= 255; delta++)
        for (int integer = 0; integer <= delta; integer++) {
            float weight = (float)integer / delta;
            float cases[] = {weight, nextafterf(weight, 0), nextafterf(weight, 1)};
            for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
                assert(palette_mix(0, (uint32_t)delta, cases[k]) == reference(0, (uint32_t)delta, cases[k]));
                assert(palette_mix((uint32_t)delta, 0, cases[k]) == reference((uint32_t)delta, 0, cases[k]));
            }
        }
}
static uint32_t seed = 1;
static uint32_t random32(void) { seed = seed * 1664525u + 1013904223u; return seed; }
static void random_colors(void) {
    for (int i = 0; i < 1000000; i++) {
        uint32_t a = random32() & 0xffffff, b = random32() & 0xffffff;
        float weight = from_bits(random32() % 0x40000000u); // positive finite, including subnormals and >=1
        assert(palette_mix(a, b, weight) == reference(a, b, weight));
    }
    assert(palette_mix(0x123456, 0xfedcba, -INFINITY) == 0x123456);
    assert(palette_mix(0x123456, 0xfedcba, INFINITY) == 0xfedcba);
}
int main(void) {
    all_mantissas(); integer_boundaries(); random_colors();
    puts("palette blend: 67108864 binary32 mantissas, integer boundaries and 1000000 colors exact");
}
