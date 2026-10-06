#include "wake_resample.h"
// Hamming-windowed 32-tap sinc at 7kHz, interpolated rate 48kHz, gain 2 (Q14).
static const int16_t k_filter[32] = {54,42,-18,-121,-181,-57,275,575,434,-337,-1331,-1581,-173,2950,6665,9187,9189,6665,2950,-173,-1581,-1331,-337,434,575,275,-57,-181,-121,-18,42,54};
size_t wake_resample(wake_resample_t *s, const int16_t *pcm, size_t n, int16_t *out) {
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        s->position = (s->position + 1) % 16;
        s->history[s->position] = pcm[i];
        for (unsigned phase = 0; phase < 2; phase++) {
            if (s->phase++ % 3) continue;
            int64_t sum = 0;
            for (unsigned k = 0; k < 16; k++)
                sum += (int32_t)s->history[(s->position + 16 - k) % 16] * k_filter[2*k + phase];
            int32_t value = (int32_t)(sum / 16384);
            out[count++] = value > 32767 ? 32767 : value < -32768 ? -32768 : (int16_t)value;
        }
    }
    s->phase %= 3;
    return count;
}
