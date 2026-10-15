#include "../main/face_math.h"
#include <assert.h>
#include <stdio.h>

static float value(uint32_t bits) { float v; memcpy(&v, &bits, sizeof v); return v; }
static void compare(float v, float low, float high) {
    assert(float_less(v, low) == (v < low));
    float actual = clampf(v, low, high), expected = v < low ? low : v > high ? high : v;
    assert(!memcmp(&actual, &expected, sizeof actual));
}
int main(void) {
    const uint32_t boundaries[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff,
        0x00800000, 0x3f800000, 0xbf800000, 0x41f00000, 0xc1f00000,
        0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00001, 0xffc00001};
    unsigned n = sizeof boundaries / sizeof *boundaries;
    for (unsigned i = 0; i < n; i++) for (unsigned j = 0; j < n; j++) for (unsigned k = 0; k < n; k++)
        compare(value(boundaries[i]), value(boundaries[j]), value(boundaries[k]));
    uint32_t seed = 39;
    for (unsigned i = 0; i < 1000000; i++) {
        seed = seed * 1664525u + 1013904223u; float v = value(seed);
        seed = seed * 1664525u + 1013904223u; float lo = value(seed);
        seed = seed * 1664525u + 1013904223u; float hi = value(seed);
        compare(v, lo, hi); compare(v, 0, 1); compare(v, -30, 30);
    }
    puts("float clamp: binary32 order and original result bits, boundaries and 1000000 triples");
}
