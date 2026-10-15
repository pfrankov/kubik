#pragma once
#include <stdint.h>
#include <string.h>

// Exact binary32 multiply/truncate for an 8-bit channel delta. A C6 has no FPU;
// this avoids three conversion/multiply/conversion calls for every channel.
// Called with a normalized 24-bit mantissa and biased exponent 119..126.
static inline int palette_delta(int delta, uint32_t mantissa, unsigned exponent) {
    if (!delta) return 0;
    unsigned magnitude = delta < 0 ? (unsigned)-delta : (unsigned)delta;
    uint32_t product = magnitude * mantissa; // <=255*(2^24-1), fits uint32
    int shift = 8 - __builtin_clz(product);
    if (shift < 0) shift = 0;
    uint32_t rounded = product >> shift;
    if (shift) {
        uint32_t remainder = product & ((1u << shift) - 1), half = 1u << (shift - 1);
        rounded += remainder > half || (remainder == half && (rounded & 1));
    }
    // Round to 24 significant bits as binary32 does, then truncate to integer.
    int result = (int)(rounded >> (150 - exponent - (unsigned)shift));
    return delta < 0 ? -result : result;
}

static inline uint32_t palette_mix(uint32_t a, uint32_t b, float weight) {
    if (weight >= 1) return b;
    // |channel delta|<=255: every product below this threshold truncates to 0.
    // Invalid NaN weights also leave the input color unchanged.
    if (!(weight > 1.f / 256)) return a;
    uint32_t bits; memcpy(&bits, &weight, sizeof bits);
    uint32_t mantissa = (bits & 0x7fffff) | 0x800000;
    unsigned exponent = (bits >> 23) & 255;
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int channel = (a >> shift) & 255, target = (b >> shift) & 255;
        out |= (uint32_t)(channel + palette_delta(target - channel, mantissa, exponent)) << shift;
    }
    return out;
}
