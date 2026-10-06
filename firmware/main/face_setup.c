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
#define SETUP_CARD_TOP 64.f
#define SETUP_CARD_BOTTOM 414.f

void draw_setup_card(face_t *f, scene_t *s) {
    int n = f->qr_n;
    int avail = (int)((SETUP_CARD_BOTTOM - SETUP_CARD_TOP) / R_SCALE);
    int module = n > 0 ? avail / (n + 4) : 0;
    if (module > 7) module = 7;
    if (n > 0 && module < 2) module = 2;
    float half = n > 0 ? (n + 4) * module * (float)R_SCALE * 0.5f : 120.f;
    float cx = CX, cy = (SETUP_CARD_TOP + SETUP_CARD_BOTTOM) * 0.5f;
    float k = clampf(f->qr_t / 0.25f, 0, 1);
    float a = k * k * (3.f - 2.f * k);
    float sc = 0.95f + 0.05f * a;
    uint32_t accent = f->qr_step == 2 ? UI_GOLD : UI_ACCENT;
    sc_label(s, TXT_HEADING, f->setup_text[0], cx, SETUP_TITLE_Y, 0, accent, a);
    sc_rbox(s, cx, cy, half * sc + 5, half * sc + 5, 22, 0, accent, a);
    sc_rbox(s, cx, cy, half * sc, half * sc, 16, 0, 0xFFFFFF, a);
    if (n > 0) {
        sc_qr(s, f->qr, n, module, cx, cy, UI_INK, a);
    } else {
        draw_wifi(s, cx, cy + 22, UI_INK, a, f->t * 3);
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(f->t * 1.6f - i * 0.25f, 1.f);
            float up = ph < 0.35f ? sinf(ph / 0.35f * PI) : 0;
            sc_circle(s, cx + (i - 1) * 22.f, cy + 70 - up * 8, 6, UI_SURFACE, a);
        }
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
    // Waiting for the approval.
    float da = clampf((t - 0.9f) / 0.4f, 0, 1);
    for (int i = 0; i < 3; i++) {
        float ph = fmodf(f->t * 1.2f - i * 0.18f, 1.f);
        float up = ph < 0.4f ? sinf(ph / 0.4f * PI) : 0;
        sc_circle(s, cx + (i - 1) * 20.f, 392 - up * 7, 5.5f, UI_ACCENT, da * (0.45f + 0.55f * up));
    }
}
