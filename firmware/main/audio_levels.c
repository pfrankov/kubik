#include "audio_levels.h"
#include <math.h>
#include <stdatomic.h>

// Compute gains only when a control changes; the audio loop uses integer math.
static atomic_int s_speech_gain = 64225; // 70%, squared curve and +6 dB
static atomic_int s_interface_gain = 11654; // 70%: logarithmic interface curve, unity at 100%
static int percent(int value) { return value < 0 ? 0 : value > 100 ? 100 : value; }
void audio_levels_set_speech(int value) {
    int p = percent(value);
    atomic_store(&s_speech_gain, (p * p * 131072LL) / 10000);
}
void audio_levels_set_interface(int value) {
    int p = percent(value);
    atomic_store(&s_interface_gain, p ? (int)lrintf(powf(10.f, (p - 100) / 40.f) * 65536.f) : 0);
}
bool audio_levels_interface_enabled(void) { return atomic_load(&s_interface_gain) != 0; }
bool audio_levels_speech_enabled(void) { return atomic_load(&s_speech_gain) != 0; }
static int16_t limit(int32_t value, int32_t knee) {
    // Smoothly reserve headroom for loud providers instead of hard clipping +6 dB peaks.
    int64_t magnitude = value < 0 ? -(int64_t)value : value;
    if (magnitude > knee) {
        int64_t excess = magnitude - knee, headroom = 32767 - knee;
        magnitude = knee + (int32_t)((int64_t)headroom * excess / (headroom + excess));
    }
    return (int16_t)(value < 0 ? -magnitude : magnitude);
}
int16_t audio_levels_limit(int32_t value) { return limit(value, 24576); }
// This knee is above the maximum speech-only output: quiet UI cannot re-limit a loud voice.
int16_t audio_levels_mix(int32_t value) { return limit(value, 32000); }
int16_t audio_levels_speech(int16_t sample) {
    return audio_levels_limit((int32_t)((int64_t)sample * atomic_load(&s_speech_gain) / 65536));
}
int32_t audio_levels_interface(int32_t sample) {
    return (int32_t)((int64_t)sample * atomic_load(&s_interface_gain) / 65536);
}
