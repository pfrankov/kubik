#include "printer_emoji.h"

#include <string.h>

bool printer_emoji_sequence_valid(const char *sequence, size_t bytes) {
    return emoji_sequence_valid(sequence, bytes);
}

static bool has_ink(const printer_emoji_glyph_t *glyph) {
    for (unsigned i = 0; i < PRINTER_EMOJI_BYTES; i++) if (glyph->bitmap[i]) return true;
    return false;
}

static bool unique(const printer_emoji_glyph_t *glyphs, unsigned at) {
    for (unsigned i = 0; i < at; i++)
        if (glyphs[i].length == glyphs[at].length &&
            !memcmp(glyphs[i].sequence, glyphs[at].sequence, glyphs[at].length)) return false;
    return true;
}

bool printer_emoji_valid(const printer_emoji_glyph_t *glyphs, unsigned count) {
    if (count > PRINTER_EMOJI_MAX || (count && !glyphs)) return false;
    for (unsigned i = 0; i < count; i++) {
        const printer_emoji_glyph_t *glyph = &glyphs[i];
        if (!glyph->length || glyph->length > PRINTER_EMOJI_SEQUENCE_MAX_BYTES) return false;
        if (strnlen(glyph->sequence, sizeof glyph->sequence) != glyph->length) return false;
        if (!printer_emoji_sequence_valid(glyph->sequence, glyph->length) || !has_ink(glyph) || !unique(glyphs, i)) return false;
    }
    return true;
}

const printer_emoji_glyph_t *printer_emoji_match(const printer_emoji_glyph_t *glyphs,
    unsigned count, const char *text, size_t bytes) {
    if (!text || count > PRINTER_EMOJI_MAX || (count && !glyphs)) return NULL;
    bool is_emoji;
    size_t cluster = emoji_cluster_next(text, bytes, &is_emoji);
    if (!is_emoji || cluster > PRINTER_EMOJI_SEQUENCE_MAX_BYTES) return NULL;
    for (unsigned i = 0; i < count; i++) {
        const printer_emoji_glyph_t *glyph = &glyphs[i];
        if (glyph->length == cluster && !memcmp(text, glyph->sequence, cluster)) return glyph;
    }
    return NULL;
}

bool printer_emoji_pixel(const printer_emoji_glyph_t *glyph, unsigned x, unsigned y) {
    if (!glyph || x >= PRINTER_EMOJI_EDGE || y >= PRINTER_EMOJI_EDGE) return false;
    return (glyph->bitmap[y * (PRINTER_EMOJI_EDGE / 8) + x / 8] & (0x80u >> (x % 8))) != 0;
}
