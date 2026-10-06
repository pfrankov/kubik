#include "ima_adpcm.h"

static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
static const int8_t INDEX_ADJ[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

int16_t ima_decode_nibble(ima_state_t *s, uint8_t n) {
    int step = STEP[s->index];
    int diff = step >> 3;
    if (n & 4) diff += step;
    if (n & 2) diff += step >> 1;
    if (n & 1) diff += step >> 2;
    int p = s->pred + ((n & 8) ? -diff : diff);
    s->pred = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
    int i = s->index + INDEX_ADJ[n & 7];
    s->index = i < 0 ? 0 : i > 88 ? 88 : i;
    return (int16_t)s->pred;
}

void ima_decode(ima_state_t *s, const uint8_t *in, size_t bytes, int16_t *out) {
    for (size_t i = 0; i < bytes; i++) {
        *out++ = ima_decode_nibble(s, in[i] & 15);
        *out++ = ima_decode_nibble(s, in[i] >> 4);
    }
}

int ima_read_header(const uint8_t *in, ima_state_t *s) {
    if (in[2] > 88) return 0;
    s->pred = (int16_t)(in[0] | in[1] << 8);
    s->index = in[2];
    return 1;
}

static uint8_t ima_encode_sample(ima_state_t *s, int16_t sample) {
    int step = STEP[s->index];
    int diff = sample - s->pred;
    uint8_t n = 0;
    if (diff < 0) { n = 8; diff = -diff; }
    if (diff >= step) { n |= 4; diff -= step; }
    step >>= 1;
    if (diff >= step) { n |= 2; diff -= step; }
    step >>= 1;
    if (diff >= step) n |= 1;
    ima_decode_nibble(s, n);  // track exactly what the decoder will reconstruct
    return n;
}

void ima_encode(ima_state_t *s, const int16_t *pcm, size_t samples, uint8_t *out) {
    out[0] = (uint8_t)s->pred; out[1] = (uint8_t)(s->pred >> 8); out[2] = (uint8_t)s->index;
    for (size_t i = 0; i < samples; i += 2) {
        uint8_t lo = ima_encode_sample(s, pcm[i]);
        out[IMA_HEADER_BYTES + i / 2] = lo | (ima_encode_sample(s, pcm[i + 1]) << 4);
    }
}
