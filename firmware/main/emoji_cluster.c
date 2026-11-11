#include "emoji_cluster.h"

typedef struct { uint32_t first, last; } range_t;

static bool within(uint32_t cp, const range_t *ranges, size_t count) {
    if (!count || cp < ranges[0].first || cp > ranges[count - 1].last) return false;
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (cp < ranges[middle].first) high = middle;
        else if (cp > ranges[middle].last) low = middle + 1;
        else return true;
    }
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

bool emoji_utf8_read(const char **cursor, const char *end, uint32_t *cp) {
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
    static const range_t starts[] = {
#include "emoji_cluster_data.inc"
    };
    return within(cp, starts, sizeof starts / sizeof *starts);
}

static bool take(const char **cursor, const char *end, uint32_t expected) {
    const char *next = *cursor;
    uint32_t cp;
    if (!emoji_utf8_read(&next, end, &cp) || cp != expected) return false;
    *cursor = next;
    return true;
}

static bool keycap(const char *cursor, const char *end) {
    take(&cursor, end, 0xfe0f);
    return take(&cursor, end, 0x20e3) && cursor == end;
}

static bool regional_pair(const char *cursor, const char *end) {
    uint32_t cp;
    return emoji_utf8_read(&cursor, end, &cp) && regional(cp) && cursor == end;
}

static bool tag_flag(const char *cursor, const char *end) {
    unsigned tags = 0;
    while (cursor < end) {
        uint32_t cp;
        if (!emoji_utf8_read(&cursor, end, &cp)) return false;
        if (cp == 0xe007f) return tags && cursor == end;
        if (cp < 0xe0020 || cp > 0xe007e) return false;
        tags++;
    }
    return false;
}

static bool component(const char **cursor, const char *end) {
    uint32_t cp;
    if (!emoji_utf8_read(cursor, end, &cp) || !emoji_base(cp) || regional(cp) || modifier(cp)) return false;
    take(cursor, end, 0xfe0f);
    const char *next = *cursor;
    if (emoji_utf8_read(&next, end, &cp) && modifier(cp)) *cursor = next;
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
    return first == 0x1f3f4 && emoji_utf8_read(&cursor, end, &cp) && cp >= 0xe0020 && cp <= 0xe007f;
}

bool emoji_sequence_valid(const char *sequence, size_t bytes) {
    if (!sequence || !bytes || bytes > EMOJI_CLUSTER_MAX_BYTES) return false;
    const char *cursor = sequence, *end = sequence + bytes;
    uint32_t first;
    if (!emoji_utf8_read(&cursor, end, &first)) return false;
    if (keycap_start(first)) return keycap(cursor, end);
    if (regional(first)) return regional_pair(cursor, end);
    if (modifier(first)) return cursor == end;
    if (tag_prefix(first, cursor, end)) return tag_flag(cursor, end);
    return joined(sequence, end);
}

static bool extender(uint32_t cp) {
    static const range_t ranges[] = {
        {0x300,0x36f}, {0x1ab0,0x1aff}, {0x1dc0,0x1dff}, {0x200c,0x200c},
        {0x20d0,0x20ff}, {0xfe00,0xfe0f}, {0xfe20,0xfe2f}, {0x1f3fb,0x1f3ff},
        {0xe0020,0xe007f}, {0xe0100,0xe01ef},
    };
    return within(cp, ranges, sizeof ranges / sizeof *ranges);
}

static bool delimiter(uint32_t cp) {
    return cp <= ' ' || cp == 0x7f || cp == 0xa0 || cp == 0x2028 || cp == 0x2029;
}

static const char *compound_tail(const char *cursor, const char *end) {
    bool joined = false;
    while (cursor < end) {
        const char *next = cursor;
        uint32_t cp;
        if (!emoji_utf8_read(&next, end, &cp)) break;
        if (cp == 0x200d) { cursor = next; joined = true; }
        else if (extender(cp)) cursor = next;
        else if (joined && !delimiter(cp)) { cursor = next; joined = false; }
        else break;
    }
    return cursor;
}

static const char *keycap_end(const char *cursor, const char *end) {
    take(&cursor, end, 0xfe0f);
    return take(&cursor, end, 0x20e3) ? cursor : NULL;
}

size_t emoji_cluster_next(const char *text, size_t bytes, bool *is_emoji) {
    if (is_emoji) *is_emoji = false;
    if (!text || !bytes) return 0;
    const char *cursor = text, *end = text + bytes;
    uint32_t first;
    if (!emoji_utf8_read(&cursor, end, &first)) return 1;
    const char *scalar_end = cursor;
    if (keycap_start(first)) {
        cursor = keycap_end(cursor, end);
        if (!cursor) return scalar_end - text;
    } else if (!emoji_base(first)) return scalar_end - text;
    else if (regional(first)) {
        const char *next = cursor; uint32_t cp;
        if (emoji_utf8_read(&next, end, &cp) && regional(cp)) cursor = next;
    }
    if (is_emoji) *is_emoji = true;
    return compound_tail(cursor, end) - text;
}
