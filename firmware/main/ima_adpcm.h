#pragma once
#include <stddef.h>
#include <stdint.h>

// IMA ADPCM, 4 bits per sample: both voice directions use a quarter of the PCM size (12 KB/s), so the
// speech ring holds 4x more audio in the same RAM and the network, TLS and Wi-Fi buffers carry 4x less.
// Byte = two samples, low nibble first. Frame on the wire (speech 0x03, microphone 0x04): int16 LE predictor, u8 step index
// (the encoder state before the frame), then the nibbles.
typedef struct { int32_t pred; int32_t index; } ima_state_t;

#define IMA_HEADER_BYTES 3

int16_t ima_decode_nibble(ima_state_t *s, uint8_t nibble);
// Decodes `bytes` bytes into 2*bytes samples.
void ima_decode(ima_state_t *s, const uint8_t *in, size_t bytes, int16_t *out);
// Reads a frame header; false when the state is out of range.
int ima_read_header(const uint8_t *in, ima_state_t *s);

// Encodes an even sample count into a 3-byte state header plus samples/2 bytes.
void ima_encode(ima_state_t *s, const int16_t *pcm, size_t samples, uint8_t *out);
