#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "emoji_cluster.h"

#define PRINTER_EMOJI_EDGE 32
#define PRINTER_EMOJI_BYTES 128
#define PRINTER_EMOJI_MAX 8
#define PRINTER_EMOJI_SEQUENCE_MAX_BYTES EMOJI_CLUSTER_MAX_BYTES

typedef struct {
    uint8_t length;
    char sequence[PRINTER_EMOJI_SEQUENCE_MAX_BYTES + 1];
    uint8_t bitmap[PRINTER_EMOJI_BYTES];
} printer_emoji_glyph_t;

bool printer_emoji_sequence_valid(const char *sequence, size_t bytes);
bool printer_emoji_valid(const printer_emoji_glyph_t *glyphs, unsigned count);
// Call with validated glyphs. Longest complete sequence; a shorter base never
// consumes a missing composite.
const printer_emoji_glyph_t *printer_emoji_match(const printer_emoji_glyph_t *glyphs,
    unsigned count, const char *text, size_t bytes);
bool printer_emoji_pixel(const printer_emoji_glyph_t *glyph, unsigned x, unsigned y);
