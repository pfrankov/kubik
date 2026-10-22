#include "audio_levels.h"
#include <math.h>
#include <stdatomic.h>

// Compute gains only when a control changes; the audio loop uses integer math.
// Gains publish no other state: sample-wise atomic reads need no ordering fence.
static atomic_int s_speech_gain = 65536; // 100%, unity
static atomic_int s_interface_gain = 65536;
static int percent(int value) { return value < 0 ? 0 : value > 100 ? 100 : value; }
// Shared taper: (p / 100)^1.5. The midpoint is -9 dB, 0 is mute, 100 is unity.
// Quiet body notes remain usable below the midpoint without increasing the ceiling.
static int gain(int value) {
    float level = percent(value) / 100.f;
    return (int)lrintf(level * sqrtf(level) * 65536.f);
}
void audio_levels_set_speech(int value) {
    atomic_store(&s_speech_gain, gain(value));
}
void audio_levels_set_interface(int value) {
    atomic_store(&s_interface_gain, gain(value));
}
bool audio_levels_interface_enabled(void) { return atomic_load_explicit(&s_interface_gain, memory_order_relaxed) != 0; }
bool audio_levels_speech_enabled(void) { return atomic_load_explicit(&s_speech_gain, memory_order_relaxed) != 0; }
static int16_t limit(int32_t value, int32_t knee) {
    // Smoothly reserve headroom for loud providers instead of hard clipping peaks.
    uint32_t magnitude = value < 0 ? 0u - (uint32_t)value : (uint32_t)value;
    if (magnitude > (uint32_t)knee) {
        uint32_t headroom = 32767 - knee, denominator = headroom + magnitude - knee;
        // h*x/(h+x) = h - ceil(h*h/(h+x)); h*h fits uint32 for both knees.
        uint32_t numerator = headroom * headroom, quotient = numerator / denominator;
        magnitude = 32767 - quotient - (numerator - quotient * denominator != 0);
    }
    return (int16_t)(value < 0 ? -(int32_t)magnitude : (int32_t)magnitude);
}
int16_t audio_levels_limit(int32_t value) { return limit(value, 24576); }
// This knee is above the maximum speech-only output: quiet UI cannot re-limit a loud voice.
int16_t audio_levels_mix(int32_t value) { return limit(value, 32000); }
int16_t audio_levels_speech(int16_t sample) {
    if (!sample) return 0;
    return audio_levels_limit((int32_t)((int32_t)sample * atomic_load_explicit(&s_speech_gain, memory_order_relaxed) / 65536));
}
int32_t audio_levels_interface(int32_t sample) {
    if (!sample) return 0;
    return (int32_t)((int64_t)sample * atomic_load_explicit(&s_interface_gain, memory_order_relaxed) / 65536);
}
