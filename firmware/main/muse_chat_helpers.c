#include "muse_chat_priv.h"
#include <stdio.h>

static void little_endian(uint8_t *out, uint32_t value, unsigned size) {
    for (unsigned i = 0; i < size; i++) out[i] = (uint8_t)(value >> (i * 8));
}
void muse_hatch_wav_header(uint8_t out[MUSE_HATCH_WAV_HEADER], uint32_t rate) {
    memcpy(out, "RIFF", 4); little_endian(out + 4, UINT32_MAX, 4);
    memcpy(out + 8, "WAVEfmt ", 8); little_endian(out + 16, 16, 4);
    little_endian(out + 20, 1, 2); little_endian(out + 22, 1, 2);
    little_endian(out + 24, rate, 4); little_endian(out + 28, rate * 2, 4);
    little_endian(out + 32, 2, 2); little_endian(out + 34, 16, 2);
    memcpy(out + 36, "data", 4); little_endian(out + 40, UINT32_MAX, 4);
}
size_t muse_hatch_base64(const uint8_t *in, size_t size, char *out) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t written = 0;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t value = (uint32_t)in[i] << 16;
        if (i + 1 < size) value |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < size) value |= in[i + 2];
        out[written++] = alphabet[(value >> 18) & 63]; out[written++] = alphabet[(value >> 12) & 63];
        out[written++] = i + 1 < size ? alphabet[(value >> 6) & 63] : '=';
        out[written++] = i + 2 < size ? alphabet[value & 63] : '=';
    }
    return written;
}
void muse_hatch_tail_words(const char *text, char *out, size_t cap) {
    snprintf(out, cap, "%s", text);
}
bool muse_hatch_caption_at(const char *text, size_t at, char *out, size_t cap) {
    (void)at; if (!text || !text[0] || !cap) return false;
    snprintf(out, cap, "%s", text); return true;
}
