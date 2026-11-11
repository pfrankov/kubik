#include "printer_emoji.h"

#include <string.h>

typedef struct { uint32_t first, last; } range_t;

static bool within(uint32_t cp, const range_t *ranges, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (cp >= ranges[i].first && cp <= ranges[i].last) return true;
    return false;
}

static unsigned utf8_head(unsigned char first, uint32_t *cp, uint32_t *minimum) {
    if (first >= 0xc2 && first <= 0xdf) { *minimum = 0x80; *cp = first & 0x1f; return 1; }
    if (first >= 0xe0 && first <= 0xef) { *minimum = 0x800; *cp = first & 0x0f; return 2; }
    if (first >= 0xf0 && first <= 0xf4) { *minimum = 0x10000; *cp = first & 7; return 3; }
    return 0;
}

static bool scalar(uint32_t cp, uint32_t minimum) {
    return cp >= minimum && cp <= 0x10ffff && (cp < 0xd800 || cp > 0xdfff);
}

bool printer_utf8_read(const char **cursor, const char *end, uint32_t *cp) {
    if (!cursor || !*cursor || !end || !cp || *cursor >= end) return false;
    const unsigned char *p = (const unsigned char *)*cursor;
    unsigned char first = *p++;
    if (first < 0x80) { *cp = first; *cursor = (const char *)p; return true; }
    uint32_t minimum;
    unsigned tail = utf8_head(first, cp, &minimum);
    if (!tail || (size_t)(end - (const char *)p) < tail) return false;
    for (unsigned i = 0; i < tail; ++i) {
        if ((*p & 0xc0) != 0x80) return false;
        *cp = (*cp << 6) | (*p++ & 0x3f);
    }
    if (!scalar(*cp, minimum)) return false;
    *cursor = (const char *)p;
    return true;
}

static bool modifier(uint32_t cp) { return cp >= 0x1f3fb && cp <= 0x1f3ff; }
static bool regional(uint32_t cp) { return cp >= 0x1f1e6 && cp <= 0x1f1ff; }

static bool emoji_base(uint32_t cp) {
    static const range_t ranges[] = {
        {0xa9,0xa9}, {0xae,0xae}, {0x203c,0x203c}, {0x2049,0x2049}, {0x2122,0x2122},
        {0x2139,0x2139}, {0x2194,0x2199}, {0x21a9,0x21aa}, {0x231a,0x231b},
        {0x2328,0x2328}, {0x23cf,0x23cf}, {0x23e9,0x23f3}, {0x23f8,0x23fa},
        {0x24c2,0x24c2}, {0x25aa,0x25ab}, {0x25b6,0x25b6}, {0x25c0,0x25c0},
        {0x25fb,0x25fe}, {0x2600,0x27bf}, {0x2934,0x2935}, {0x2b05,0x2b07},
        {0x2b1b,0x2b1c}, {0x2b50,0x2b50}, {0x2b55,0x2b55}, {0x3030,0x3030},
        {0x303d,0x303d}, {0x3297,0x3297}, {0x3299,0x3299}, {0x1f000,0x1faff},
    };
    return !modifier(cp) && !regional(cp) && within(cp, ranges, sizeof ranges / sizeof *ranges);
}

static bool take(const char **cursor, const char *end, uint32_t expected) {
    const char *next = *cursor;
    uint32_t cp;
    if (!printer_utf8_read(&next, end, &cp) || cp != expected) return false;
    *cursor = next;
    return true;
}

static bool keycap(const char *cursor, const char *end) {
    take(&cursor, end, 0xfe0f);
    return take(&cursor, end, 0x20e3) && cursor == end;
}

static bool regional_pair(const char *cursor, const char *end) {
    uint32_t cp;
    return printer_utf8_read(&cursor, end, &cp) && regional(cp) && cursor == end;
}

static bool tag_flag(const char *cursor, const char *end) {
    unsigned tags = 0;
    while (cursor < end) {
        uint32_t cp;
        if (!printer_utf8_read(&cursor, end, &cp)) return false;
        if (cp == 0xe007f) return tags && cursor == end;
        if (cp < 0xe0020 || cp > 0xe007e) return false;
        tags++;
    }
    return false;
}

static bool component(const char **cursor, const char *end) {
    uint32_t cp;
    if (!printer_utf8_read(cursor, end, &cp) || !emoji_base(cp)) return false;
    take(cursor, end, 0xfe0f);
    const char *next = *cursor;
    if (printer_utf8_read(&next, end, &cp) && modifier(cp)) *cursor = next;
    return true;
}

static bool joined(const char *cursor, const char *end) {
    if (!component(&cursor, end)) return false;
    while (cursor < end)
        if (!take(&cursor, end, 0x200d) || !component(&cursor, end)) return false;
    return true;
}

static bool keycap_start(uint32_t cp) { return cp == '#' || cp == '*' || (cp >= '0' && cp <= '9'); }

static bool tag_prefix(uint32_t first, const char *cursor, const char *end) {
    uint32_t cp;
    return first == 0x1f3f4 && printer_utf8_read(&cursor, end, &cp) && cp >= 0xe0020 && cp <= 0xe007f;
}

bool printer_emoji_sequence_valid(const char *sequence, size_t bytes) {
    if (!sequence || !bytes || bytes > PRINTER_EMOJI_SEQUENCE_MAX_BYTES) return false;
    const char *cursor = sequence, *end = sequence + bytes;
    uint32_t first;
    if (!printer_utf8_read(&cursor, end, &first)) return false;
    if (keycap_start(first)) return keycap(cursor, end);
    if (regional(first)) return regional_pair(cursor, end);
    if (tag_prefix(first, cursor, end)) return tag_flag(cursor, end);
    return joined(sequence, end);
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

static bool continuation(const char *text, const char *end) {
    static const range_t ranges[] = {
        {0x300,0x36f}, {0x1ab0,0x1aff}, {0x1dc0,0x1dff}, {0x200c,0x200d},
        {0x20d0,0x20ff}, {0xfe00,0xfe0f}, {0xfe20,0xfe2f}, {0x1f3fb,0x1f3ff},
        {0xe0020,0xe007f}, {0xe0100,0xe01ef},
    };
    if (text == end) return false;
    uint32_t cp;
    if (!printer_utf8_read(&text, end, &cp)) return true;
    return within(cp, ranges, sizeof ranges / sizeof *ranges);
}

const printer_emoji_glyph_t *printer_emoji_match(const printer_emoji_glyph_t *glyphs,
    unsigned count, const char *text, size_t bytes) {
    if (!text || count > PRINTER_EMOJI_MAX || (count && !glyphs)) return NULL;
    const printer_emoji_glyph_t *best = NULL;
    for (unsigned i = 0; i < count; i++) {
        const printer_emoji_glyph_t *glyph = &glyphs[i];
        unsigned length = glyph->length;
        if (!length || length > PRINTER_EMOJI_SEQUENCE_MAX_BYTES || length > bytes) continue;
        if (best && length <= best->length) continue;
        if (memcmp(text, glyph->sequence, length) || continuation(text + length, text + bytes)) continue;
        best = glyph;
    }
    return best;
}

bool printer_emoji_pixel(const printer_emoji_glyph_t *glyph, unsigned x, unsigned y) {
    if (!glyph || x >= PRINTER_EMOJI_EDGE || y >= PRINTER_EMOJI_EDGE) return false;
    return (glyph->bitmap[y * (PRINTER_EMOJI_EDGE / 8) + x / 8] & (0x80u >> (x % 8))) != 0;
}
