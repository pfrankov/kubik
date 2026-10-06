// Text is set from one table of roles (font.h): the fonts kern, every role centres its capitals on the y it is
// given, and the words the firmware shows fit the shapes they sit in. cc via tools/test-render.py
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../main/face.h"
#include "../main/ui_text.h"

static float width_by_hand(const text_style_t *st, const char *s) {
    const font_t *f = &g_fonts[st->font];
    int w = 0, prev = -1, n = 0;
    for (const char *p = s; *p; n++) {
        int g = font_glyph(f, font_utf8_next(&p));
        w += f->g[g].adv + (prev >= 0 ? font_kern(f, prev, g) : 0);
        prev = g;
    }
    return (w + (n - 1) * (int)lrintf(st->track * 16)) / 16.f;
}

static void kerning_tables(void) {
    for (int i = 0; i < FONT_COUNT; i++) {
        const font_t *f = &g_fonts[i];
        for (int k = 0; k < f->n_kern; k++) {
            assert((f->kern_pair[k] >> 8) < f->n && (f->kern_pair[k] & 255) < f->n);
            assert(k == 0 || f->kern_pair[k - 1] < f->kern_pair[k]);  // sorted, no repeats
            assert(font_kern(f, f->kern_pair[k] >> 8, f->kern_pair[k] & 255) == f->kern_q4[k]);
        }
        assert(f->cap > 0 && f->cap < f->ascent);
    }
    const font_t *f = &g_fonts[FONT_STRONG];
    int a = font_glyph(f, 'A'), v = font_glyph(f, 'V'), l = font_glyph(f, 'L'), o = font_glyph(f, 'o');
    assert(font_kern(f, a, v) < -16 && font_kern(f, v, a) < 0);  // AV and VA close up
    assert(font_kern(f, l, o) == 0 || font_kern(f, l, o) > -32);
    printf("kerning: AV %d/16 px, tables sorted (", font_kern(f, a, v));
    for (int i = 0; i < FONT_COUNT; i++) printf("%s%d", i ? "+" : "", g_fonts[i].n_kern);
    printf(" pairs)\n");
}

// A label's box, as the scene has it, matches the width the layout code asks for.
static void label_widths_and_centres(void) {
    static scene_t s;
    for (int role = 0; role < TXT_ROLES; role++) {
        const text_style_t *st = &g_text[role];
        const char *word = st->font == FONT_CODE ? "K7QX" : "Hamburgefons 0123";
        scene_begin(&s, 0);
        prim_t *p = sc_label(&s, (text_role_t)role, word, 100, 200, -1, 0xFFFFFF, 1);
        assert(p && fabsf(label_width((text_role_t)role, word) - width_by_hand(st, word)) < .07f);
        const font_t *f = &g_fonts[st->font];
        const font_glyph_t *h = &f->g[font_glyph(f, 'H')];
        if (st->font == FONT_CODE) h = &f->g[font_glyph(f, 'K')];
        float top = p->sub_y - h->top, middle = top + (h->h - 1) * .5f;  // the ink of a capital
        assert(fabsf(middle - 200) <= 1.01f);
        assert(p->bx1 - p->bx0 >= (int)label_width((text_role_t)role, word));
        assert(st->line >= f->cap + 6 && st->line <= 2 * (f->ascent + f->descent));
    }
    printf("labels: every role's capitals centred on their y within 1 px\n");
}

static void upper(char *out, const char *in) {  // as face_bubble does for ASCII
    for (; *in; in++) *out++ = (char)toupper((unsigned char)*in);
    *out = 0;
}

static void words_fit(void) {
    float widest = 0;
    for (int i = 0; i < STR_COUNT; i++) {
        char w[64];
        upper(w, str((str_id_t)i));
        const char *p = w;
        while (*p) assert(font_utf8_next(&p) != '?');
        float tw = label_width(TXT_BUBBLE, w);
        widest = fmaxf(widest, tw);
        assert(tw <= 371);  // bubble: icon disc + padding + text inside 440 px of the 480 wide panel
    }
    // The buttons of the menu's bottom row (k_actions in face_menu.c): 24 px of symbol, 8 of gap, the word, and at
    // least 8 px to each rounded end.
    static const struct { str_id_t word; float width; } k_actions[3] = {{STR_WIFI, 126}, {STR_RESET, 130}, {STR_POWER_OFF, 160}};
    float tightest = 1e9f;
    for (int i = 0; i < 3; i++) tightest = fminf(tightest, k_actions[i].width - 24 - 8 - label_width(TXT_ACTION, str(k_actions[i].word)));
    printf("words: widest bubble text %.0f px (of 371), tightest button leaves %.0f px around symbol and word (of 16 needed)\n", widest, tightest);
    assert(tightest >= 16);
    char chip[80];
    upper(chip, "Kubik-4F2A  \xC2\xB7  password kubik4f2abcd");
    printf("setup chip with the longest password: %.0f px\n", label_width(TXT_CHIP, chip));
    assert(label_width(TXT_CHIP, chip) + 32 <= 447);  // the chip's ends are circles of 17 px: inside the 84 px corners from 447 px wide
    upper(chip, "Scan again for setup");
    assert(label_width(TXT_HEADING, chip) <= 440);
}

// A setup decoration must never steal the QR's previously available space.
static void setup_qr_room(void) {
    static const int sizes[] = {21, 25, 29, 33, 37, 41};
    static const int modules[] = {7, 6, 5, 4, 4, 3};
    static uint8_t qr[41 * 41];
    static scene_t scene;
    for (int step = 1; step <= 2; step++) {
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            face_t f; face_init(&f); face_set_mode(&f, MODE_SETUP);
            face_set_setup(&f, qr, sizes[i], step, step == 1 ? "Scan to join Wi-Fi" : "Scan again for setup", "192.168.4.1");
            f.qr_t = 1; face_draw(&f, &scene);
            int codes = 0, lines = 0;
            for (int j = 0; j < scene.n; j++) {
                const prim_t *p = &scene.p[j];
                if (p->kind == PK_TEXT) lines++;
                if (p->kind != PK_QR) continue;
                codes++;
                assert(p->b == modules[i]);
                assert(p->bx1 - p->bx0 + 1 == sizes[i] * modules[i] * R_SCALE);
                assert(p->by0 >= 64 && p->by1 < 414);
            }
            assert(codes == 1 && lines == 2); // existing heading + detail only
        }
    }
}

// Independent reference: measure each whole candidate as the old layout did.
static size_t prefix_reference(text_role_t role, const char *text, size_t cap, float width, const char *suffix) {
    size_t fit = 0;
    for (const char *p = text; *p;) {
        font_utf8_next(&p);
        size_t n = (size_t)(p - text);
        if (n > cap) break;
        char candidate[256];
        memcpy(candidate, text, n); strcpy(candidate + n, suffix);
        if (label_width(role, candidate) > width) break;
        fit = n;
    }
    return fit;
}
static void prefix_fitting(void) {
    const char *texts[] = {"", "AVATAR To ff...", "Claude Sonnet 4.6 Extended Thinking",
        "UTF-8: café Привет 日本語", "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii"};
    for (int role = 0; role < TXT_ROLES; role++) for (unsigned t = 0; t < sizeof texts / sizeof texts[0]; t++) {
        for (size_t cap = 0; cap <= strlen(texts[t]); cap++) for (int width = 0; width < 420; width += 7) {
            assert(label_prefix(role, texts[t], cap, width + .25f, "...") == prefix_reference(role, texts[t], cap, width + .25f, "..."));
            assert(label_prefix(role, texts[t], cap, width + .25f, "") == prefix_reference(role, texts[t], cap, width + .25f, ""));
        }
    }
}


static bool is_page_marker(const prim_t *p) {
    return p->kind == PK_RBOX && p->r > 0 && p->a < 100 && p->cy != 414 * 16;
}

static void check_live_card_scene(const scene_t *s, bool live) {
    int footer = 0, lines = 0, pages = 0;
    for (int i = 0; i < s->n; i++) {
        const prim_t *p = &s->p[i];
        assert(p->bx0 >= 0); assert(p->bx1 < 480);
        assert(p->by0 >= 0); assert(p->by1 < 480);
        if (p->kind == PK_TEXT && p->by0 >= 399) footer++;
        if (p->kind == PK_TEXT && p->r == FONT_CARD) {
            lines++; if (live) assert(p->by1 < 389);
        }
        if (is_page_marker(p)) {
            pages++; if (live) assert((p->cy + p->b) / 16 + 2 < 389);
        }
    }
    assert(footer == (live ? 2 : 0));
    assert(lines == FACE_CARD_PAGE && pages == 2);
}

// Live controls must survive long text pages and their entrance/fade; Classic keeps its layout.
static void live_card_controls(void) {
    const face_mode_t modes[] = {MODE_LISTENING, MODE_THINKING, MODE_SPEAKING, MODE_THINKING};
    for (int live = 0; live < 2; live++) for (int mode = 0; mode < 4; mode++) {
        face_t f; face_init(&f); face_set_character(&f, CHARACTER_TESS);
        f.live_active = live; f.live_ready = mode != 3; f.live_mic = mode != 3;
        f.charging = f.usb_power = f.battery_present = mode == 3; f.battery_pct = 60;
        face_set_mode(&f, modes[mode]);
        face_card(&f, "First line\nSecond line\nThird line\nFourth line\nFifth line\nSixth line\nSeventh line");
        f.card_hold = true;
        for (int frame = 1; frame <= 25; frame++) {
            f.card_t = frame / 60.f;
            f.card_left = frame > 20 ? (26 - frame) / 60.f : -1;
            scene_t s; face_draw(&f, &s);
            check_live_card_scene(&s, live);
        }
    }
    puts("Live: six-line text, page markers and persistent controls clear throughout entrance/fade; Classic contrast passed");
}

// Listening stays unmistakable in silence; level follows the microphone, never a timer.
static int live_meter(float level, bool ready, bool mic) {
    face_t f; face_init(&f); face_set_character(&f, CHARACTER_TESS);
    f.live_active = true; f.live_ready = ready; f.live_mic = mic; f.mic_level = level;
    face_set_mode(&f, MODE_LISTENING);
    for (int i = 0; i < 90; i++) face_update(&f, 1.f / 30);
    scene_t s; face_draw(&f, &s);
    int bars = 0, height = 0, badge = 0;
    for (int i = 0; i < s.n; i++) {
        const prim_t *p = &s.p[i];
        if (p->kind != PK_RBOX || p->cy != 414 * 16) continue;
        if (p->a == 170 * 16) badge++;
        else if (p->a == 40) {
            bars++; height += p->b;
            assert((p->cx - p->a) / 16 >= 310 && (p->cx + p->a) / 16 < 380);
            assert((p->cy - p->b) / 16 - 2 >= 399 && (p->cy + p->b) / 16 + 2 < 430);
        }
    }
    assert(badge == (ready && mic ? 1 : 0));
    assert(bars == (ready && mic ? 7 : 0));
    return height;
}
static void live_recording_meter(void) {
    int quiet = live_meter(0, true, true);
    assert(quiet > 0 && live_meter(.2f, true, true) > quiet);
    assert(live_meter(1, true, true) > live_meter(.2f, true, true));
    assert(live_meter(1, false, true) == 0);
    assert(live_meter(1, true, false) == 0);
}

int main(void) {
    live_card_controls();
    live_recording_meter();
    prefix_fitting();
    kerning_tables();
    label_widths_and_centres();
    words_fit();
    setup_qr_room();
    puts("text layout ok");
    return 0;
}
