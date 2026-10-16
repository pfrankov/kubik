#include "face_internal.h"
#include "ui_theme.h"

// The card is set in capitals: Latin and Cyrillic, in place (same byte length).
static void upper_utf8(char *s) {
    for (uint8_t *p = (uint8_t *)s; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
        else if (p[0] == 0xD0 && p[1] >= 0xB0 && p[1] <= 0xBF) p[1] -= 0x20, p++;           // а..п
        else if (p[0] == 0xD1 && p[1] >= 0x80 && p[1] <= 0x8F) p[0] = 0xD0, p[1] += 0x20, p++;  // р..я
        else if (p[0] == 0xD1 && p[1] == 0x91) p[0] = 0xD0, p[1] = 0x81, p++;                  // ё
    }
}

void face_set_setup(face_t *f, const uint8_t *mods, int n, int step, const char *title, const char *detail) {
    const char *in[2] = {title, detail};
    if (!title || !title[0]) mods = NULL, n = 0, step = 0;
    bool was = f->setup_text[0][0] != 0, is = title && title[0];
    if (was != is || f->qr_step != step || (f->qr == NULL) != (mods == NULL)) f->qr_t = 0;
    f->qr = mods;
    f->qr_n = mods ? n : 0;
    f->qr_step = step;
    for (int i = 0; i < 2; i++) {
        const char *t = is && in[i] ? in[i] : "";
        strncpy(f->setup_text[i], t, sizeof f->setup_text[i] - 1);
        f->setup_text[i][sizeof f->setup_text[i] - 1] = 0;
        upper_utf8(f->setup_text[i]);
    }
}

#define COL_CHIP UI_SURFACE

// Keep all available room for the QR. Step two changes the existing heading
// and accent; it does not add another row or reduce the camera target.
#define SETUP_TITLE_Y 37.f
#define SETUP_CHIP_Y 440.f
#define SETUP_CARD_TOP 54.f
#define SETUP_CARD_BOTTOM 426.f
#define QR_INK 0xE6ECEAu  // nearly white on the black screen: no bright card to throw off a phone camera's exposure

// Connecting: a ring with Tess's point running round it, a long trail behind (Plush: a turning arc).
static void draw_connecting(face_t *f, scene_t *s, float cx, float cy, float a, uint32_t accent) {
    const float R = 104;
    float spin = f->t * 3.4f;
    sc_ring(s, cx, cy, R, 3, UI_SURFACE, a);
    if (face_character(f) == CHARACTER_TESS) {
        for (int k = 12; k >= 1; k--) {
            float fade = (13 - k) / 13.f * a, ang = spin - k * .12f;
            sc_glow_dot(s, cx + R * cosf(ang), cy + R * sinf(ang), 3 + fade * 4, rgb_scale(accent, fade * .9f), (uint8_t)(fade * 150));
        }
        float tw = .5f + .5f * sinf(f->t * 9);
        sc_glow_dot(s, cx + R * cosf(spin), cy + R * sinf(spin), 7 + 2 * tw, rgb_mix(accent, UI_GOLD, tw * .5f), (uint8_t)(a * 255));
    } else {
        sc_arc(s, cx, cy, R, .7f, 6, spin - PI / 2, accent, a);
    }
    draw_wifi(s, cx, cy + 14, accent, a, f->t * 3);
}

// Tess celebrates a joined network with a ring of sparks that fly out from the disc and fade.
static void draw_sparks(face_t *f, scene_t *s, float cx, float cy) {
    float t = clampf((f->qr_t - .2f) / 1.2f, 0, 1), out = 1 - (1 - t) * (1 - t) * (1 - t);
    if (f->qr_t < .2f || t >= 1) return;  // they leave the disc as it pops, and are gone once faded
    for (int i = 0; i < 10; i++) {
        float angle = i * (2 * PI / 10) + .3f, r = 105 + 100 * out * (.8f + .2f * (i & 1)), fade = 1 - t;
        sc_glow_dot(s, cx + r * cosf(angle), cy + r * sinf(angle), 2 + 3 * fade, rgb_mix(UI_ACCENT, UI_GOLD, (i & 1) * .8f + .2f),
                    (uint8_t)(220 * fade));
    }
}

// Result: a disc with the check (joined) or the alert (failed); it pops in.
static void draw_result(face_t *f, scene_t *s, float cx, float cy, float a, uint32_t accent) {
    float k = clampf(f->qr_t / .5f, 0, 1), pop = face_character(f) == CHARACTER_TESS ? .5f + .5f * (1 - (1 - k) * (1 - k) * (1 + 2.2f * k)) : 1.f;
    sc_circle(s, cx, cy, 84 * pop, accent, a);
    sc_icon(s, f->qr_step == 4 ? ICON_CHECK : ICON_CIRCLE_ALERT, cx, cy, 100 * pop, UI_INK, a);
    if (f->qr_step == 4 && face_character(f) == CHARACTER_TESS) draw_sparks(f, s, cx, cy);
}

void draw_setup_card(face_t *f, scene_t *s) {
    int n = f->qr_n;
    int avail = (int)((SETUP_CARD_BOTTOM - SETUP_CARD_TOP) / R_SCALE);
    int module = n > 0 ? avail / (n + 2) : 0;  // a module of black margin each side; the screen is the rest of the quiet zone
    if (module > 8) module = 8;
    if (n > 0 && module < 2) module = 2;
    float cx = CX, cy = (SETUP_CARD_TOP + SETUP_CARD_BOTTOM) * 0.5f;
    float k = clampf(f->qr_t / 0.3f, 0, 1);
    float a = k * k * (3.f - 2.f * k);
    uint32_t accent = f->qr_step == 2 ? UI_GOLD : f->qr_step == 5 ? UI_ERROR : UI_ACCENT;
    sc_label(s, TXT_HEADING, f->setup_text[0], cx, SETUP_TITLE_Y, 0, accent, a);
    if (n > 0) {  // the code is drawn light on black: a phone reads it inverted
        sc_qr(s, f->qr, n, module, cx, cy, QR_INK, a);
    } else if (f->qr_step >= 4) {
        draw_result(f, s, cx, cy, a, accent);
    } else {
        draw_connecting(f, s, cx, cy, a, accent);
    }
    if (f->setup_text[1][0]) {
        float w = label_width(TXT_CHIP, f->setup_text[1]);
        sc_rbox(s, cx, SETUP_CHIP_Y, w * 0.5f + 16, 17, 17, 0, COL_CHIP, a);
        sc_label(s, TXT_CHIP, f->setup_text[1], cx, SETUP_CHIP_Y, 0, accent, a);
    }
}

void face_set_pairing(face_t *f, const char *code) {
    if (!code) code = "";
    if (strncmp(f->pair_code, code, sizeof f->pair_code - 1)) f->qr_t = 0;
    strncpy(f->pair_code, code, sizeof f->pair_code - 1);
    f->pair_code[sizeof f->pair_code - 1] = 0;
}

void face_bubble(face_t *f, bubble_icon_t icon, const char *text, float seconds) {
    // The same bubble again only extends its time: no second pop.
    bool same = f->bub_icon == (int)icon && icon && !strncmp(f->bub_text, text ? text : "", sizeof f->bub_text - 1);
    if (!same) f->bub_t = 0;
    f->bub_icon = icon;
    f->bub_left = seconds;
    strncpy(f->bub_text, text ? text : "", sizeof f->bub_text - 1);
    f->bub_text[sizeof f->bub_text - 1] = 0;
    upper_utf8(f->bub_text);
}

#define COL_BUBBLE 0x22292Bu
#define BUBBLE_Y 44.f
#define BUBBLE_R 21.f  // icon disc radius

void draw_bubble_icon(scene_t *s, int icon, float x, float y, float r, float t, float a) {
    static const uint32_t disc[BUB_COUNT] = {0,         COL_REC,  COL_GOLD, COL_BLUE, COL_GREY, COL_GREY, COL_MINT,
                                             COL_MINT,  COL_GOLD, COL_REC,  COL_BLUE, COL_MINT, COL_MINT, COL_GREY,
                                             COL_BLUE,  COL_BLUE, COL_BLUE, COL_BLUE, COL_BLUE, COL_BLUE,
                                             COL_GREY};
    static const uint8_t glyph[BUB_COUNT] = {
        [BUB_ERROR] = ICON_CIRCLE_ALERT,
        [BUB_NOT_HEARD] = ICON_CIRCLE_HELP,
        [BUB_BUSY] = ICON_HOURGLASS,
        [BUB_NO_WIFI] = ICON_WIFI_OFF,
        [BUB_NO_SERVER] = ICON_UNPLUG,
        [BUB_TALK] = ICON_MIC,
        [BUB_KEY] = ICON_KEY_ROUND,
        [BUB_PLUGIN] = ICON_PLUG,
        [BUB_REC] = ICON_MIC,
        [BUB_THINK] = ICON_BRAIN,
        [BUB_OK] = ICON_CHECK,
        [BUB_VOLUME] = ICON_VOLUME_2,
        [BUB_STOP] = ICON_SQUARE,
        [BUB_TOOL] = ICON_WRENCH,
        [BUB_CODE] = ICON_CODE_XML,
        [BUB_WEB] = ICON_GLOBE,
        [BUB_DEPLOY] = ICON_UPLOAD,
        [BUB_BUILD] = ICON_BLOCKS,
        [BUB_COMPACT] = ICON_FOLD_HORIZONTAL,
        [BUB_POWER] = ICON_POWER,
    };
    if (icon <= BUB_NONE || icon >= BUB_COUNT) return;
    const uint32_t ink = COL_INK;
    const uint32_t badge = disc[icon];
    sc_circle(s, x, y, r, badge, a);
    if (icon == BUB_REC) {
        float pulse = 0.5f + 0.5f * sinf(t * 6.f);
        sc_ring(s, x, y, r + 3.f + 3.f * pulse, 2.5f, COL_REC, a * (0.5f - 0.3f * pulse));
    }
    float size = r * 1.12f;
    if (icon == BUB_THINK) size *= 1.f + 0.035f * sinf(t * 5.f);
    else if (icon == BUB_BUSY) size *= 1.f + 0.025f * sinf(t * 3.5f);
    sc_icon(s, glyph[icon], x, y, size, ink, a);
}

// Pairing: a key badge, the command to run where OpenClaw lives, and the code
// as eight letter tiles (4 + 4) that pop in one after another. The dots below
// keep breathing until the owner approves.
void draw_pair_card(face_t *f, scene_t *s) {
    const float cx = CX;
    float t = f->qr_t;
    float a = clampf(t / 0.25f, 0, 1);
    // Key badge with a soft mint halo.
    float kk = clampf(t / 0.4f, 0, 1);
    float kpop = 1.f - (1.f - kk) * (1.f - kk) * (1.f + 2.2f * kk);
    float breathe = 0.5f + 0.5f * sinf(f->t * 2.1f);
    prim_t *halo = sc_circle(s, cx, 92, 44, UI_ACCENT, a * (0.14f + 0.12f * breathe));
    pr_soft(halo, 22);
    sc_icon(s, ICON_KEY_ROUND, cx, 92, 48 * (0.7f + 0.3f * kpop), UI_ACCENT, a);
    float ta = clampf((t - 0.15f) / 0.3f, 0, 1);
    sc_label(s, TXT_HEADING, "PAIR WITH AGENT", cx, 157, 0, UI_TEXT, ta);
    // The command, as it is typed.
    const char *cmd = "Approve this code on the server";
    float w = label_width(TXT_CAPTION, cmd);
    sc_rbox(s, cx, 214, w * 0.5f + 18, 19, 19, 0, COL_CHIP, ta);
    sc_label(s, TXT_CAPTION, cmd, cx, 214, 0, UI_ACCENT, ta);
    // The code: one tile per character.
    int n = (int)strlen(f->pair_code);
    if (n > 8) n = 8;
    const float tw = 23, th = 32, gap = 7, mid = 14;
    float total = n * 2 * tw + (n - 1) * gap + (n > 4 ? mid : 0);
    float x = cx - total * 0.5f + tw;
    for (int i = 0; i < n; i++) {
        float k = clampf((t - 0.3f - i * 0.06f) / 0.3f, 0, 1);
        float pop = 1.f - (1.f - k) * (1.f - k) * (1.f + 2.4f * k);
        float ty = 296 + (1 - pop) * 14;
        sc_rbox(s, x, ty, tw * (0.85f + 0.15f * pop), th * (0.85f + 0.15f * pop), 12, 0, UI_TEXT, k);
        char ch[2] = {f->pair_code[i], 0};
        sc_label(s, TXT_CODE, ch, x, ty, 0, UI_INK, clampf((k - 0.3f) / 0.7f, 0, 1));
        x += 2 * tw + gap + (i == 3 ? mid : 0);
    }
    // The command uses the exact code shown above; no list lookup is needed.
    bool hermes = f->pair_hermes;
    sc_label(s, TXT_CAPTION, hermes ? "On your Hermes server, approve:" : "On the OpenClaw server, run:", cx, 360, 0, UI_TEXT, ta);
    char approval[64];
    snprintf(approval, sizeof approval, hermes ? "python <home>/plugins/kubik/pair.py %.8s"
             : "openclaw pairing approve kubik %.8s", f->pair_code);
    sc_label(s, TXT_CAPTION, approval, cx, 390, 0, UI_ACCENT, ta);
    // Waiting for the approval.
    float da = clampf((t - 0.9f) / 0.4f, 0, 1);
    for (int i = 0; i < 3; i++) {
        float ph = fmodf(f->t * 1.2f - i * 0.18f, 1.f);
        float up = ph < 0.4f ? sinf(ph / 0.4f * PI) : 0;
        sc_circle(s, cx + (i - 1) * 20.f, 442 - up * 7, 5.5f, UI_ACCENT, da * (0.45f + 0.55f * up));
    }
}
