#include "font.h"

// `line` is baseline to baseline: 1.25 em, which only the card (several lines) uses.
const text_style_t g_text[TXT_ROLES] = {
    [TXT_BUBBLE] = {FONT_STRONG, 26, 1.f},
    [TXT_HEADING] = {FONT_TITLE, 33, 1.5f},
    [TXT_CHIP] = {FONT_STRONG, 26, .25f},  // the longest one (network and 8-digit password) must clear the round corners
    [TXT_CAPTION] = {FONT_STRONG, 26, .5f},
    [TXT_ACTION] = {FONT_STRONG, 26, .3f},
    [TXT_CHOICE] = {FONT_TITLE, 33, .3f},
    [TXT_NUMBER] = {FONT_BIG, 52, 0.f},
    [TXT_CODE] = {FONT_CODE, 60, 0.f},
    [TXT_CARD] = {FONT_CARD, 40, 0.f},
    [TXT_PAGE_TITLE] = {FONT_BIG, 48, 0.f},
    [TXT_PAGE_BODY] = {FONT_CARD, 40, 0.f},
};

uint32_t font_utf8_next(const char **s) {
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0x80 && !more) c = '?';
    else if (more) c &= 0x3F >> more;
    for (; more > 0; more--) {
        if ((*p & 0xC0) != 0x80) { c = '?'; break; }
        c = (c << 6) | (*p++ & 0x3F);
    }
    *s = (const char *)p;
    return c;
}

int font_glyph(const font_t *f, uint32_t cp) {
    if (cp == 0xA0) cp = ' ';
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->g[mid].cp == cp) return mid;
        if (f->g[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return cp == '?' ? 0 : font_glyph(f, '?');
}

int font_kern(const font_t *f, int first, int second) {
    unsigned key = (unsigned)(first << 8 | second);
    int lo = 0, hi = f->n_kern - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->kern_pair[mid] == key) return f->kern_q4[mid];
        if (f->kern_pair[mid] < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}
