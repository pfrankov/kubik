#include "ui_kit.h"
#include "ui_theme.h"
#include <string.h>

void ui_fit(char *out, size_t cap, const char *src, text_role_t role, float max_width) {
    if (!cap) return;
    src = src ? src : "";
    size_t n = label_prefix(role, src, cap - 1, max_width, "");
    const char *suffix = "";
    if (src[n] && cap >= 4) {
        n = label_prefix(role, src, cap - 4, max_width, "...");
        suffix = "...";
    }
    memcpy(out, src, n);
    strcpy(out + n, suffix);
}

#define CHIP_H 18.f
#define CHIP_PAD 14.f
#define CHIP_DOT 5.f
void ui_chip(scene_t *s, const char *text, float x, float y, int align, uint32_t color) {
    char shown[40];
    ui_fit(shown, sizeof shown, text, TXT_CAPTION, 150);
    float w = label_width(TXT_CAPTION, shown) + 2 * CHIP_PAD + 2 * CHIP_DOT + 6;
    float left = align > 0 ? x - w : align < 0 ? x : x - w * .5f;
    sc_rbox(s, left + w * .5f, y, w * .5f, CHIP_H + 4, CHIP_H + 4, 0, UI_SURFACE, 1);
    sc_circle(s, left + CHIP_PAD + CHIP_DOT, y, CHIP_DOT, color, 1);
    sc_label(s, TXT_CAPTION, shown, left + CHIP_PAD + 2 * CHIP_DOT + 6, y, -1, UI_TEXT, 1);
}

void ui_header(scene_t *s, const char *title, const char *chip, uint32_t chip_color) {
    sc_label(s, TXT_PAGE_TITLE, title, 40, 42, -1, UI_TEXT, 1);
    if (chip && chip[0]) ui_chip(s, chip, 440, 42, 1, chip_color);
}

#define SEG_LEFT 20.f
#define SEG_RIGHT 460.f
#define SEG_GAP 8.f
#define SEG_HH 24.f
static float seg_width(unsigned count) { return (SEG_RIGHT - SEG_LEFT - SEG_GAP * (count - 1)) / count; }

void ui_segments(scene_t *s, float y, const char *const *labels, unsigned count, unsigned current) {
    if (!count) return;
    float w = seg_width(count);
    for (unsigned i = 0; i < count; i++) {
        bool on = i == current;
        float cx = SEG_LEFT + w * .5f + i * (w + SEG_GAP);
        sc_rbox(s, cx, y, w * .5f, SEG_HH, SEG_HH, 0, on ? UI_GOLD : UI_SURFACE, 1);
        sc_label(s, TXT_CHOICE, labels[i], cx, y, 0, on ? UI_INK : UI_MUTED, 1);
    }
}

int ui_segment_hit(unsigned count, int x, int y, float row_y) {
    if (!count || y < row_y - 36 || y > row_y + 36 || x < SEG_LEFT || x > SEG_RIGHT) return -1;
    float w = seg_width(count);
    unsigned i = (unsigned)((x - SEG_LEFT) / (w + SEG_GAP));
    return (int)(i < count ? i : count - 1);
}

void ui_enter(scene_t *s, int first, float elapsed) {
    float t = elapsed / UI_ENTER_SECONDS;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float ease = 1 - (1 - t) * (1 - t) * (1 - t);
    if (ease >= 1.f) return;
    for (int i = first; i < s->n; i++) s->p[i].alpha = (uint8_t)(s->p[i].alpha * ease);
}
