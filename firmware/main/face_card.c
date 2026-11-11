#include "face_internal.h"
#include "viewport.h"
#include "emoji_cluster.h"

// ------------------------------------------------------------------ text card
#define COL_PLUSH 0xF2EADCu
#define COL_INK 0x15171Bu
#define CARD_TEXT_X 38.f    // left edge of the text, design px
#define CARD_TEXT_W 404.f
#define CARD_CY (UI_CY + 28.f)

// Reading time of a page: a couple of seconds plus ~15 characters a second.
static float card_page_secs(const face_t *f) {
    const char *line = f->card_buf + f->card_page_start[f->card_page];
    int bytes = 0;
    for (int i = 0; i < f->card_page_count[f->card_page]; i++) {
        int n = (int)strlen(line); bytes += n; line += n + 1;
    }
    return clampf(2.5f + bytes * 0.5f * 0.07f, 4.f, 20.f);  // bytes/2: mostly two-byte Cyrillic
}

static uint32_t card_hash(const char *s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

typedef struct {
    face_t *f;
    int o;          // write position in card_buf
    int ll, nonempty;
} card_wrap_t;

static void card_flush(card_wrap_t *w) {
    face_t *f = w->f;
    if (f->card_n >= FACE_CARD_LINES || w->o + w->ll + 1 > (int)sizeof f->card_buf) { w->ll = 0; return; }
    f->card_n++;
    if (w->ll) w->nonempty = f->card_n;
    f->card_buf[w->o + w->ll] = 0;
    w->o += w->ll + 1;
    w->ll = 0;
}

static bool card_fits(card_wrap_t *w, const char *add, int n, bool space) {
    if (w->o + w->ll + n + 2 > (int)sizeof w->f->card_buf) return false;
    char *line = w->f->card_buf + w->o;
    int k = w->ll;
    if (space) line[k++] = ' ';
    memcpy(line + k, add, n);
    line[k + n] = 0;
    face_emoji_key_t keys[4];
    unsigned count = 0;
    bool fits = rich_label_width(TXT_CARD, line) <= CARD_TEXT_W &&
                face_emoji_collect(line, k + n, keys, &count);
    line[w->ll] = 0;
    return fits;
}

static void card_append(card_wrap_t *w, const char *add, int n, bool space) {
    char *line = w->f->card_buf + w->o;
    if (space) line[w->ll++] = ' ';
    memcpy(line + w->ll, add, n);
    w->ll += n;
    line[w->ll] = 0;
}

// Keep words when possible; split long words only at whole cluster boundaries.
static void card_word(card_wrap_t *w, const char *word, int n) {
    if (card_fits(w, word, n, w->ll > 0)) { card_append(w, word, n, w->ll > 0); return; }
    if (w->ll) card_flush(w);
    while (n > 0) {
        if (card_fits(w, word, n, false)) { card_append(w, word, n, false); return; }
        int take = 0;  // complete UTF-8 scalars / emoji compounds that still fit
        for (;;) {
            bool emoji;
            int step = (int)emoji_cluster_next(word + take, n - take, &emoji);
            if (!step || take + step > n || !card_fits(w, word, take + step, false)) break;
            take += step;
        }
        if (!take) return;  // a single glyph wider than the card: give up on the word
        card_append(w, word, take, false);
        card_flush(w);
        word += take;
        n -= take;
    }
}

static void card_wrap(face_t *f, const char *text) {
    card_wrap_t w = {.f = f};
    f->card_n = 0;
    for (const char *p = text; *p;) {
        if (*p == '\n') {
            card_flush(&w);  // also keeps an empty line between paragraphs
            p++;
            continue;
        }
        if (*p == ' ' || *p == '\t') { p++; continue; }
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        card_word(&w, p, (int)(e - p));
        p = e;
    }
    if (w.ll) card_flush(&w);
    f->card_n = w.nonempty; // Trim trailing empty lines without rescanning the pool.
}

const char *face_card_line(const face_t *f, int index) {
    if (index < 0 || index >= f->card_n) return NULL;
    const char *line = f->card_buf;
    for (int i = 0; i < index; i++) line += strlen(line) + 1;
    return line;
}

static void card_paginate(face_t *f) {
    face_emoji_key_t keys[4];
    unsigned count = 0;
    f->card_pages = 0;
    int offset = 0;
    for (int i = 0; i < f->card_n; i++) {
        const char *line = f->card_buf + offset;
        int page = f->card_pages - 1;
        bool full = page < 0 || f->card_page_count[page] == FACE_CARD_PAGE;
        if (full || !face_emoji_collect(line, strlen(line), keys, &count)) {
            page = f->card_pages++;
            f->card_page_start[page] = (uint16_t)offset;
            f->card_page_count[page] = 0;
            count = 0;
            face_emoji_collect(line, strlen(line), keys, &count);
        }
        f->card_page_count[page]++;
        offset += (int)strlen(line) + 1;
    }
}

void face_card(face_t *f, const char *text) {
    int len = text ? (int)strlen(text) : 0;
    if (!len) {
        if (f->card_n && f->card_left < 0) f->card_left = 0.35f;  // fade out
        return;
    }
    bool grows = f->card_n && f->card_left != 0 && len >= f->card_src_len &&
                 card_hash(text, f->card_src_len) == f->card_hash;
    card_wrap(f, text);
    card_paginate(f);
    f->card_hash = card_hash(text, len);
    f->card_src_len = len;
    if (!f->card_n) return;
    if (!grows) {
        face_emoji_reset(f);
        f->card_t = 0;
        f->card_page = 0;
        f->card_page_t = 0;
    }
    if (f->card_page >= f->card_pages) f->card_page = f->card_pages - 1;
    f->card_left = -1;
}

bool face_card_tap(face_t *f) {
    if (!f->card_n || f->card_left == 0) return false;
    if (f->card_page + 1 < f->card_pages) {
        f->card_page++;
        f->card_page_t = 0;
    } else if (f->card_left < 0 || f->card_left > 0.35f) {
        f->card_left = 0.35f;
    }
    return true;
}

void card_update(face_t *f, float dt) {
    if (!f->card_n) return;
    face_emoji_update(f, dt);
    f->card_t += dt;
    f->card_page_t += dt;
    if (f->card_left > 0) {
        if ((f->card_left -= dt) <= 0) {
            f->card_n = f->card_pages = 0;
            f->card_left = 0;
            face_emoji_reset(f);
        }
        return;
    }
    if (f->card_page_t < card_page_secs(f)) return;
    if (f->card_page + 1 < f->card_pages) {
        f->card_page++;
        f->card_page_t = 0;
    } else if (!f->card_hold && f->card_page_t > card_page_secs(f) + 6.f) {
        f->card_left = 0.35f;  // read and a little more: goes away by itself
    }
}

static float card_center(const face_t *f, float pop) {
    // Reserve the Live footer even while a full six-line card enters or fades.
    return f->live_active ? UI_CY - 20 + (1.f - pop) * 8 : CARD_CY + (1.f - pop) * 28;
}

void draw_card(face_t *f, scene_t *s) {
    if (!f->card_n) return;
    float k = clampf(f->card_t / 0.35f, 0, 1);
    float pop = 1.f - (1.f - k) * (1.f - k) * (1.f + 1.8f * k);
    float a = clampf(f->card_t / 0.2f, 0, 1);
    if (f->card_left > 0) a *= clampf(f->card_left / 0.35f, 0, 1);
    if (a < 0.01f) return;
    face_emoji_sync(f);
    scene_emoji_snapshot(s, &f->card_emoji.glyphs);
    const char *line = f->card_buf + f->card_page_start[f->card_page];
    int n = f->card_page_count[f->card_page];
    int pages = f->card_pages;
    // The card keeps the height of a full page once the text has more than one.
    int rows = pages > 1 ? FACE_CARD_PAGE : n;
    float hh = (rows * g_text[TXT_CARD].line + 30.f) * 0.5f;
    float cy = card_center(f, pop);
    sc_rbox(s, CX, UI_CY, UI_W / 2, UI_H / 2, 0, 0, 0x000000u, a * 0.55f);  // the face steps back
    sc_rbox(s, CX, cy, CARD_TEXT_W * 0.5f + 20, hh, 26, 0, COL_PLUSH, a);
    float middle = cy - hh + 15 + g_text[TXT_CARD].line * .5f;  // 15 px of air above the first line
    for (int i = 0; i < n; i++) {
        if (*line) sc_rich_label(s, TXT_CARD, line, CARD_TEXT_X, middle + i * g_text[TXT_CARD].line, -1, COL_INK, a);
        line += strlen(line) + 1;
    }
    if (pages > 1) {  // which page: dots under the card
        int shown = pages < 8 ? pages : 8;
        for (int i = 0; i < shown; i++) {
            bool on = i == (f->card_page < shown ? f->card_page : shown - 1);
            sc_circle(s, CX + (i - (shown - 1) * 0.5f) * 16.f, cy + hh + 16, on ? 4.5f : 3.f, on ? COL_PLUSH : 0x9A948Bu, a);
        }
    }
}
