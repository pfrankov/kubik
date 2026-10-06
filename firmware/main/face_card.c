#include "face_internal.h"

// ------------------------------------------------------------------ text card
#define COL_PLUSH 0xF2EADCu
#define COL_INK 0x15171Bu
#define CARD_TEXT_X 38.f    // left edge of the text, design px
#define CARD_TEXT_W 404.f
#define CARD_CY 268.f

static int card_pages(const face_t *f) { return (f->card_n + FACE_CARD_PAGE - 1) / FACE_CARD_PAGE; }

// Reading time of a page: a couple of seconds plus ~15 characters a second.
static float card_page_secs(const face_t *f) {
    int first = f->card_page * FACE_CARD_PAGE, bytes = 0;
    for (int i = first; i < f->card_n && i < first + FACE_CARD_PAGE; i++) bytes += (int)strlen(f->card_buf + f->card_line[i]);
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
    char line[160]; // the line being filled
    int ll;
} card_wrap_t;

static void card_flush(card_wrap_t *w) {
    face_t *f = w->f;
    if (f->card_n >= FACE_CARD_LINES || w->o + w->ll + 1 > (int)sizeof f->card_buf) { w->ll = 0; return; }
    f->card_line[f->card_n++] = (uint16_t)w->o;
    memcpy(f->card_buf + w->o, w->line, w->ll);
    f->card_buf[w->o + w->ll] = 0;
    w->o += w->ll + 1;
    w->ll = 0;
}

static bool card_fits(card_wrap_t *w, const char *add, int n, bool space) {
    if (w->ll + n + 2 > (int)sizeof w->line) return false;
    char tmp[sizeof w->line];
    int k = w->ll;
    memcpy(tmp, w->line, k);
    if (space) tmp[k++] = ' ';
    memcpy(tmp + k, add, n);
    tmp[k + n] = 0;
    return label_width(TXT_CARD, tmp) <= CARD_TEXT_W;
}

static void card_append(card_wrap_t *w, const char *add, int n, bool space) {
    if (space) w->line[w->ll++] = ' ';
    memcpy(w->line + w->ll, add, n);
    w->ll += n;
}

// One word: onto this line, onto the next, or cut by letters when it is wider than a line (links).
static void card_word(card_wrap_t *w, const char *word, int n) {
    if (card_fits(w, word, n, w->ll > 0)) { card_append(w, word, n, w->ll > 0); return; }
    if (w->ll) card_flush(w);
    while (n > 0) {
        if (card_fits(w, word, n, false)) { card_append(w, word, n, false); return; }
        int take = 0;  // whole UTF-8 characters that still fit
        for (;;) {
            int step = 1;
            while (take + step < n && ((uint8_t)word[take + step] & 0xC0) == 0x80) step++;
            if (take + step > n || !card_fits(w, word, take + step, false)) break;
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
    while (f->card_n && !f->card_buf[f->card_line[f->card_n - 1]]) f->card_n--;  // trailing empty lines
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
    f->card_hash = card_hash(text, len);
    f->card_src_len = len;
    if (!f->card_n) return;
    if (!grows) {
        f->card_t = 0;
        f->card_page = 0;
        f->card_page_t = 0;
    }
    if (f->card_page >= card_pages(f)) f->card_page = card_pages(f) - 1;
    f->card_left = -1;
}

bool face_card_tap(face_t *f) {
    if (!f->card_n || f->card_left == 0) return false;
    if (f->card_page + 1 < card_pages(f)) {
        f->card_page++;
        f->card_page_t = 0;
    } else if (f->card_left < 0 || f->card_left > 0.35f) {
        f->card_left = 0.35f;
    }
    return true;
}

void card_update(face_t *f, float dt) {
    if (!f->card_n) return;
    f->card_t += dt;
    f->card_page_t += dt;
    if (f->card_left > 0) {
        if ((f->card_left -= dt) <= 0) {
            f->card_n = 0;
            f->card_left = 0;
        }
        return;
    }
    if (f->card_page_t < card_page_secs(f)) return;
    if (f->card_page + 1 < card_pages(f)) {
        f->card_page++;
        f->card_page_t = 0;
    } else if (!f->card_hold && f->card_page_t > card_page_secs(f) + 6.f) {
        f->card_left = 0.35f;  // read and a little more: goes away by itself
    }
}

static float card_center(const face_t *f, float pop) {
    // Reserve the Live footer even while a full six-line card enters or fades.
    return f->live_active ? 220 + (1.f - pop) * 8 : CARD_CY + (1.f - pop) * 28;
}

void draw_card(face_t *f, scene_t *s) {
    if (!f->card_n) return;
    float k = clampf(f->card_t / 0.35f, 0, 1);
    float pop = 1.f - (1.f - k) * (1.f - k) * (1.f + 1.8f * k);
    float a = clampf(f->card_t / 0.2f, 0, 1);
    if (f->card_left > 0) a *= clampf(f->card_left / 0.35f, 0, 1);
    if (a < 0.01f) return;
    int first = f->card_page * FACE_CARD_PAGE;
    int n = (int)fminf(f->card_n - first, FACE_CARD_PAGE);
    int pages = card_pages(f);
    // The card keeps the height of a full page once the text has more than one.
    int rows = pages > 1 ? FACE_CARD_PAGE : n;
    float hh = (rows * g_text[TXT_CARD].line + 30.f) * 0.5f;
    float cy = card_center(f, pop);
    sc_rbox(s, CX, 240, 240, 240, 0, 0, 0x000000u, a * 0.55f);  // the face steps back
    sc_rbox(s, CX, cy, CARD_TEXT_W * 0.5f + 20, hh, 26, 0, COL_PLUSH, a);
    float middle = cy - hh + 15 + g_text[TXT_CARD].line * .5f;  // 15 px of air above the first line
    for (int i = 0; i < n; i++) {
        const char *line = f->card_buf + f->card_line[first + i];
        if (*line) sc_label(s, TXT_CARD, line, CARD_TEXT_X, middle + i * g_text[TXT_CARD].line, -1, COL_INK, a);
    }
    if (pages > 1) {  // which page: dots under the card
        int shown = pages < 8 ? pages : 8;
        for (int i = 0; i < shown; i++) {
            bool on = i == (f->card_page < shown ? f->card_page : shown - 1);
            sc_circle(s, CX + (i - (shown - 1) * 0.5f) * 16.f, cy + hh + 16, on ? 4.5f : 3.f, on ? COL_PLUSH : 0x9A948Bu, a);
        }
    }
}
