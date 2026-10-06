#include "face_internal.h"
#include "ui_theme.h"

// ------------------------------------------------------------------ settings menu
// Full screen control centre: narrow sliders, three large navigation targets,
// Wi-Fi setup, hidden reset and power off.
// The tiles' corners follow the panel's rounded corners. Opening fades the character to black first, then the tiles
// pop in; the two are never drawn together (they would not fit the primitive budget).
#define COL_MENU_PRESSED 0x2A3739u
#define COL_TILE 0x162022u
#define COL_TILE_POWER 0x2B1818u
#define MENU_SWAP 0.45f  // menu.k where the character is gone and the tiles start
#define TILE_R 44.f
#define SLIDER_PAD 16.f  // the value reaches 0 / 100 this far inside the tile

typedef struct { float cx, cy, hw, hh; } tile_t;
// 20 px outer inset, 12 px gutters. The header leaves both status and navigation room.
//   Settings                          battery  (y  16..56)
//   volume | brightness | Agent                (y  68..164)
//                       | Guide                (y 178..274)
//                       | Status               (y 288..384)
//   Wi-Fi | [reset] | power off                 (y 396..460)
static const tile_t k_tiles[] = {
    [MENU_VOLUME] = {72, 226, 52, 158},
    [MENU_BRIGHT] = {188, 226, 52, 158},
    [MENU_AGENT] = {356, 116, 104, 48},
    [MENU_GUIDE] = {356, 226, 104, 48},
    [MENU_STATUS] = {356, 336, 104, 48},
};
static const tile_t k_events = {272, 36, 54, 28};
static const tile_t k_sound[2] = {{132, 226, 100, 158}, {348, 226, 100, 158}};
static const tile_t k_actions[3] = {{83, 428, 63, 32}, {223, 428, 65, 32}, {380, 428, 80, 32}};
static const tile_t k_public_actions[3] = {{127, 428, 107, 32}, {0}, {353, 428, 107, 32}};

static void status_value(scene_t *s, const char *value, float y, unsigned lines, text_role_t role) {
    const char *at = value[0] ? value : "Unavailable";
    for (unsigned i = 0; i < lines && *at; i++) {
        size_t n = label_prefix(role, at, 159, 424, "");
        if (!n) break;
        char line[160]; memcpy(line, at, n); line[n] = 0;
        sc_label(s, role, line, 28, y + i * 26, -1, UI_TEXT, 1);
        at += n;
    }
}
static void status_row(scene_t *s, const char *label, const char *value, float y) {
    sc_label(s, TXT_CAPTION, label, 28, y, -1, UI_MUTED, 1);
    status_value(s, value, y + 28, 2, TXT_ACTION);
}
void draw_status(const device_status_t *status, scene_t *s) {
    sc_label(s, TXT_CHOICE, "Status", 28, 36, -1, UI_TEXT, 1);
    static const char *const pages[] = {"Network / 1 of 3", "Device / 2 of 3", "Agent / 3 of 3"};
    sc_label(s, TXT_CAPTION, pages[status->page % 3], 452, 36, 1, UI_GOLD, 1);
    if (!status->page) {
        const char *state = status->connected ? "Connected" : status->radio ? "Not connected" : "Radio sleeping";
        status_row(s, "Wi-Fi", state, 88);
        status_row(s, status->connected ? "Network name" : "Saved network", status->ssid, 158);
        status_row(s, "IP address", status->connected ? status->ip : "Not connected", 228);
        status_row(s, "Gateway", status->connected ? status->gateway : "Not connected", 298);
        status_row(s, "DNS server", status->connected ? status->dns : "Not connected", 368);
    } else if (status->page == 1) {
        status_row(s, "Device name", status->name, 88);
        status_row(s, "Processor", "ESP32-C6", 158);
        status_row(s, "Firmware", status->firmware, 228);
        status_row(s, "DHCP name", status->hostname, 298);
        status_row(s, "Wi-Fi MAC", status->mac, 368);
    } else {
        const char *route = !strcmp(status->route, "wifi") ? "Wi-Fi" : !strcmp(status->route, "usb") ? "USB" : "None";
        status_row(s, "Agent", status->agent ? "Connected" : "Not connected", 88);
        status_row(s, "Connection", route, 148);
        sc_label(s, TXT_CAPTION, "Server address", 28, 208, -1, UI_MUTED, 1);
        status_value(s, status->url[0] ? status->url : "Automatic LAN discovery", 236, 8, TXT_CAPTION);
    }
    sc_label(s, TXT_CAPTION, "Swipe or tap / BOOT returns", 240, 452, 0, UI_ACCENT, 1);
}

static const tile_t *action_tile(const face_menu_t *menu, int part) {
    if (menu->service) return &k_actions[part];
    return part == 1 ? NULL : &k_public_actions[part];
}

static bool tile_hit(const tile_t *tile, int x, int y) {
    return tile && fabsf(x - tile->cx) <= tile->hw && fabsf(y - tile->cy) <= tile->hh;
}

static float tile_r(const tile_t *t) { return fminf(TILE_R, fminf(t->hw, t->hh)); }

int face_menu_slider(int y) {
    const tile_t *t = &k_tiles[MENU_VOLUME];
    float y0 = t->cy - t->hh + SLIDER_PAD, y1 = t->cy + t->hh - SLIDER_PAD;
    return (int)lrintf(clampf((y1 - y) / (y1 - y0), 0, 1) * 100);
}

static int tile_part(int row, int y) {
    switch (row) {
    case MENU_VOLUME:
    case MENU_BRIGHT: return face_menu_slider(y);
    case MENU_AGENT: return 0;
    case MENU_GUIDE: return 0;
    default: return 0;
    }
}

static bool events_hit(const face_menu_t *menu, int x, int y) {
    return menu->service && tile_hit(&k_events, x, y);
}

int face_menu_hit(const face_t *f, int x, int y, int *part) {
    *part = 0;
    if (!f->menu.open || f->menu.k < 1.f) return -1;
    if (f->menu.sound) {
        for (int i = 0; i < 2; i++) if (tile_hit(&k_sound[i], x, y)) {
            *part = face_menu_slider(y); return i ? MENU_UI_VOLUME : MENU_VOLUME;
        }
        return -1;
    }
    if (events_hit(&f->menu, x, y)) return MENU_EVENTS;
    for (int r = 0; r < MENU_UI_VOLUME; r++) {
        if (r == MENU_ACTIONS || !tile_hit(&k_tiles[r], x, y)) continue;
        *part = tile_part(r, y);
        return r;
    }
    for (int i = 0; i < 3; i++) {
        if (!tile_hit(action_tile(&f->menu, i), x, y)) continue;
        *part = i;
        return MENU_ACTIONS;
    }
    return -1;
}

// A tile popping in (k 0..1) and briefly squeezed when pressed.
static void draw_tile(scene_t *s, const tile_t *t, float k, bool pressed, float pressed_t, uint32_t c) {
    float pop = 1.f - (1.f - k) * (1.f - k) * (1.f + 1.8f * k);
    float sq = pressed ? 1.f - 0.035f * clampf(1.f - pressed_t / 0.25f, 0, 1) : 1.f;
    float sc = (0.9f + 0.1f * pop) * sq;
    bool lit = pressed && pressed_t < 0.3f && c == COL_TILE;
    sc_rbox(s, t->cx, t->cy, t->hw * sc, t->hh * sc, tile_r(t) * sc, 0, lit ? COL_MENU_PRESSED : c, clampf(k * 1.6f, 0, 1));
}

static void icon_speaker(scene_t *s, float x, float y, int level, uint32_t c, float a) {
    sc_icon(s, level > 0 ? ICON_VOLUME_2 : ICON_VOLUME_X, x, y, 25, c, a);
}

static void icon_sun(scene_t *s, float x, float y, int level, uint32_t c, float a) {
    sc_icon(s, ICON_SUN, x, y, 25 + 2.f * level / 100.f, c, a);
}

void face_set_power(face_t *f, bool present, int percent, bool charging, bool usb_power) {
    f->battery_present = present && percent >= 0 && percent <= 100;
    f->battery_pct = f->battery_present ? percent : -1;
    f->charging = charging;
    f->usb_power = usb_power;
    f->battery_low = f->battery_present && percent <= 15;
}

// A lightning bolt, centred on x, y (h: half height).
static void icon_bolt(scene_t *s, float x, float y, float h, uint32_t c, float a) {
    sc_icon(s, ICON_ZAP, x, y, h * 2.2f, c, a);
}

void draw_battery(face_t *f, scene_t *s, bool menu) {
    if (!menu && f->charging) {
        // Leave the central status bubble and the cron indicator their own space.
        icon_bolt(s, 36, 36, 10, COL_MINT, 1);
        return;
    }
    if (!menu && !f->battery_low) return;
    char text[64];
    if (!f->battery_present) snprintf(text, sizeof text, "—");
    else snprintf(text, sizeof text, "%d%%", f->battery_pct);
    float y = !menu && (f->cron_k > .01f || f->offline_k > .01f) ? CRON_Y + 50.f : 36.f;
    bool external_power = menu && f->usb_power;
    uint32_t color = external_power ? COL_MINT : f->battery_low ? COL_CORAL : COL_PLUSH;
    sc_label(s, TXT_ACTION, text, 432, y, 1, color, 1);
    if (external_power) icon_bolt(s, 432 - label_width(TXT_ACTION, text) - 12, y + 1, 10, color, 1);
}

static const char *menu_text(const face_menu_t *m, int i) {
    return m->txt[i] ? m->txt[i] : "";
}

static float menu_progress(float k, int i) {
    // Animate one tall slider at a time: simultaneous full-tile updates exceed
    // the 33 ms frame slot on the native QSPI panel. All tiles finish at k=1.
    float start = i < 2 ? i * 0.28f : 0.5f + (i - 2) * 0.12f;
    float duration = i < 2 ? 0.26f : 0.14f;
    return clampf((k - start) / duration, 0, 1);
}

static void draw_slider_readout(scene_t *s, const tile_t *t, int row, int value,
                                float top, float bottom, float alpha, bool pressed, float pressed_t) {
    float number_alpha = pressed ? clampf((1.4f - pressed_t) / 0.3f, 0, 1) : 0;
    if (number_alpha > 0) {
        char num[8]; snprintf(num, sizeof num, "%d", value);
        float y = t->cy - t->hh + 44;
        sc_label(s, TXT_NUMBER, num, t->cx, y, 0, top < y - 22 ? COL_INK : COL_PLUSH,
                 alpha * smooth01(number_alpha));
    }
    float iy = bottom - 44;
    uint32_t color = top < iy - 4 ? COL_INK : COL_PLUSH;
    if (row == MENU_VOLUME) sc_icon(s, ICON_AUDIO_LINES, t->cx, iy, 25, color, alpha);
    else if (row == MENU_UI_VOLUME) icon_speaker(s, t->cx + 2, iy, value, color, alpha);
    else icon_sun(s, t->cx, iy, value, color, alpha);
}

static int slider_value(const face_menu_t *m, int row) {
    if (row == MENU_VOLUME) return m->volume;
    return row == MENU_UI_VOLUME ? m->ui_volume : m->brightness;
}

static void draw_menu_sliders(scene_t *s, const face_menu_t *m, float k, float pt) {
    // Sliders: the level fills the tile from the bottom; the number on top, the icon at the bottom.
    for (int r = MENU_VOLUME; r <= MENU_BRIGHT; r++) {
        int row = m->sound && r == MENU_BRIGHT ? MENU_UI_VOLUME : r;
        const tile_t *t = m->sound ? &k_sound[r] : &k_tiles[r];
        float tk = menu_progress(k, r - MENU_VOLUME), a = clampf(tk * 1.6f, 0, 1);
        bool pr = m->pressed == row && pt < 0.3f;
        draw_tile(s, t, tk, false, pt, COL_TILE);
        int v = slider_value(m, row);
        uint32_t fill = m->sound || r == MENU_VOLUME ? COL_MINT : COL_GOLD;
        float bottom = t->cy + t->hh, top = bottom - 2 * t->hh * clampf(v / 100.f, 0, 1);
        if (v > 0) {
            prim_t *clip = sc_mask_rbox(s, t->cx, t->cy, t->hw, t->hh, TILE_R, 0);
            prim_t *lvl = sc_rbox(s, t->cx, (top + bottom) * 0.5f + 20, t->hw + 4, (bottom - top) * 0.5f + 20, 0, 0, fill,
                                  a * (pr ? 1.f : 0.9f));
            if (clip && lvl) pr_clip(s, lvl, clip);
        }
        draw_slider_readout(s, t, row, v, top, bottom, a, m->pressed == row, pt);

    }
}

#define ACTION_SYMBOL_HW 12.f  // the symbols are drawn within 12 px of their centre
#define ACTION_GAP 8.f         // symbol to word

static void draw_action_symbol(scene_t *s, float x, float y, int part, uint32_t color, float alpha) {
    if (part == 2) {  // power
        sc_icon(s, ICON_POWER, x, y, 24.f, color, alpha);
        return;
    }
    if (part == 1) {  // reset: a ring open at the right, with an arrow head on its upper end
        sc_arc(s, x, y, 10, 2.5f, 3.f, PI / 2, color, alpha);
        sc_capsule(s, x + 5, y - 10, x + 12, y - 10, 3.f, color, alpha);
        sc_capsule(s, x + 5, y - 10, x + 5, y - 17, 3.f, color, alpha);
        return;
    }
    sc_icon(s, ICON_QR_CODE, x, y, 25.f, color, alpha);
}

static void draw_menu_actions(scene_t *s, const face_menu_t *m, float k, float pt) {
    // Rare actions use compact, full-width touch targets; the two that cannot be undone ask twice.
    static const int k_words[3] = {MT_WIFI, MT_RESET, MT_POWER};
    for (int i = 0; i < 3; i++) {
        const tile_t *t = action_tile(m, i);
        if (!t) continue;
        float tk = menu_progress(k, 4), a = clampf(tk * 1.6f, 0, 1);
        bool pr = m->pressed == MENU_ACTIONS && m->pressed_part == i;
        bool armed = m->armed == i + 1;
        draw_tile(s, t, tk, pr, pt, armed ? COL_CORAL : i ? COL_TILE_POWER : COL_TILE);
        uint32_t c = armed ? COL_INK : i ? COL_CORAL : COL_PLUSH;
        const char *word = menu_text(m, k_words[i]);
        float left = t->cx - (2 * ACTION_SYMBOL_HW + ACTION_GAP + label_width(TXT_ACTION, word)) * 0.5f;
        draw_action_symbol(s, left + ACTION_SYMBOL_HW, t->cy, i, i == 0 && !armed ? COL_MINT : c, a);
        sc_label(s, TXT_ACTION, word, left + 2 * ACTION_SYMBOL_HW + ACTION_GAP, t->cy, -1, c, a);
    }
}

static void draw_navigation(scene_t *s, const face_menu_t *m, float k) {
    static const struct { int row, icon; const char *label; uint32_t color; } links[] = {
        {MENU_AGENT, ICON_BRAIN, "Agent", COL_MINT},
        {MENU_GUIDE, ICON_CIRCLE_HELP, "Guide", COL_GOLD},
        {MENU_STATUS, ICON_GLOBE, "Status", COL_MINT},
    };
    for (unsigned i = 0; i < sizeof links / sizeof links[0]; i++) {
        const tile_t *t = &k_tiles[links[i].row];
        float tk = menu_progress(k, 2 + i), a = clampf(tk * 1.6f, 0, 1);
        draw_tile(s, t, tk, m->pressed == links[i].row, m->pressed_t, COL_TILE);
        float width = 32 + 12 + label_width(TXT_CHOICE, links[i].label);
        float left = t->cx - width * .5f;
        sc_icon(s, links[i].icon, left + 16, t->cy, 32, links[i].color, a);
        sc_label(s, TXT_CHOICE, links[i].label, left + 44, t->cy, -1, COL_PLUSH, a);
    }
}

void draw_menu(face_t *f, scene_t *s) {
    const face_menu_t *m = &f->menu;
    float k = clampf((m->k - MENU_SWAP) / (1 - MENU_SWAP), 0, 1);
    if (k <= 0) return;
    float pt = m->pressed_t;
    draw_menu_sliders(s, m, k, pt);
    if (!m->sound) { draw_menu_actions(s, m, k, pt); draw_navigation(s, m, k); }
    else {
        sc_label(s, TXT_ACTION, "Speech", k_sound[0].cx, 424, 0, COL_MINT, 1);
        sc_label(s, TXT_ACTION, "Interface", k_sound[1].cx, 424, 0, COL_MINT, 1);
    }
    sc_label(s, TXT_ACTION, m->sound ? "Sound" : "Settings", 52, 36, -1, COL_PLUSH, 1);
    if (!m->sound && m->service) {
        sc_rbox(s, 272, 36, 54, 28, 22, 0, COL_TILE, 1);
        sc_label(s, TXT_ACTION, "Events", 272, 36, 0, COL_GOLD, 1);
    }
    draw_battery(f, s, true);
}

// The character dims to black while the menu opens (and back while it closes).
void draw_menu_fade(face_t *f, scene_t *s) {
    float k = clampf(f->menu.k / MENU_SWAP, 0, 1);
    if (k > 0.004f) sc_rbox(s, CX, 240, 240, 240, 0, 0, 0x000000u, smooth01(k));
}

// Cron jobs, top-right: a clock whose hands spin while a job runs; the countdown to the next one-shot job
// (a reminder) under it.
void draw_cron(face_t *f, scene_t *s) {
    float a = smooth01(f->cron_k);
    if (a < 0.01f) return;
    bool running = f->cron_running > 0;
    uint32_t c = running ? COL_GOLD : COL_BLUE;
    float x = CRON_X, y = CRON_Y; // shared top/right anchor
    if (running) {
        prim_t *h = sc_circle(s, x, y, 30, c, a * (0.16f + 0.10f * sinf(f->t * 3.f)));
        pr_soft(h, 12);
    }
    sc_ring(s, x, y, 19, 4.5f, c, a);
    float am = running ? f->t * 4.f : -PI / 2, ah = running ? f->t * 0.35f : PI / 6 - PI / 2;
    sc_capsule(s, x, y, x + cosf(am) * 12.f, y + sinf(am) * 12.f, 4, c, a);
    sc_capsule(s, x, y, x + cosf(ah) * 7.5f, y + sinf(ah) * 7.5f, 4.5f, c, a);
    sc_circle(s, x, y, 3, c, a);
    if (f->cron_due >= 0) {
        float left = f->cron_due - f->t;
        char buf[12];
        if (left <= 0) buf[0] = 0;
        else if (left < 60) snprintf(buf, sizeof buf, "%ds", (int)ceilf(left));
        else if (left < 3600) snprintf(buf, sizeof buf, "%dm", (int)ceilf(left / 60));
        else snprintf(buf, sizeof buf, "%dh", (int)lrintf(left / 3600));
        if (buf[0]) sc_label(s, TXT_CAPTION, buf, x, y + 37, 0, c, a);
    }
}

// On top of everything: slides down with a small pop, fades out at the end.
void draw_bubble(face_t *f, scene_t *s) {
    if (!f->bub_icon) return;
    float k = clampf(f->bub_t / 0.35f, 0, 1);
    float pop = 1.f - (1.f - k) * (1.f - k) * (1.f + 1.8f * k);
    float a = clampf(f->bub_t / 0.15f, 0, 1);
    if (f->bub_left > 0) a *= clampf(f->bub_left / 0.35f, 0, 1);
    if (a < 0.01f) return;
    float y = BUBBLE_Y - (1 - pop) * 26;
    float tw = f->bub_text[0] ? label_width(TXT_BUBBLE, f->bub_text) : 0;
    float pad = 5, hh = BUBBLE_R + pad;
    float hw = tw > 0 ? (2 * BUBBLE_R + tw + 3 * pad + 12) * 0.5f : hh;
    sc_rbox(s, CX, y, hw, hh, hh, 0, COL_BUBBLE, a);
    float ix = CX - hw + pad + BUBBLE_R;
    draw_bubble_icon(s, f->bub_icon, ix, y, BUBBLE_R, f->bub_t, a);
    if (tw > 0) sc_label(s, TXT_BUBBLE, f->bub_text, ix + BUBBLE_R + 10 + tw * 0.5f, y, 0, COL_PLUSH, a);
}
