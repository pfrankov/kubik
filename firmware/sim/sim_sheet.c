// Simulator sheet: every emotion, mode, bubble, card, menu and setup tile.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "../main/qr.h"

typedef struct {
    const char *label;
    face_mode_t mode;
    int emo;
    float mic, spk;
    int ev;
} tile_t;
static void configure_menu(face_t *f, int ev) {
    if (ev != 100 && ev != 104 && ev != 105) return;
    static const char *const words[MT_COUNT] = {"Wi-Fi", "Power off", "Reset"};
    memcpy(f->menu.txt, words, sizeof words); f->menu.open = true;
    f->menu.volume = 60; f->menu.brightness = 80;
    f->menu.pressed = MENU_VOLUME, f->menu.pressed_part = 0, f->menu.pressed_t = -12;
    if (ev == 104) {
        f->menu.volume = 5, f->menu.brightness = 100, f->menu.pressed = -1;
        f->menu.armed = 3;
    }
}
static void configure_setup(face_t *f, int ev, const uint8_t *qr_join, int n_join, const uint8_t *qr_page, int n_page) {
    if (ev == -2) face_set_setup(f, qr_join, n_join, 1, "Scan to join Wi-Fi", "KUBIK-C634  \u00B7  password 48210937");
    if (ev == -3) face_set_setup(f, qr_page, n_page, 2, "Scan again for setup", "192.168.4.1");
    if (ev == -4) face_set_setup(f, NULL, 0, 3, "Connecting\u2026", "\u201CPavel Home 5G\u201D");
    if (ev == -5) face_set_pairing(f, "7KQ2XM9P");
}
static void configure_cards(face_t *f, int ev, const char *const bub_text[BUB_COUNT]) {
    if (ev == -6) face_card(f, "Код подтверждения:\nABCD-1234");
    if (ev == -7) face_card(f, "Список покупок на завтра:\n• молоко, два литра\n• хлеб бородинский\n• яйца, десяток\n"
        "• сыр для пиццы\n• помидоры черри\n• базилик\n• оливковое масло\n• кофе в зёрнах\n• бананы");
    if (ev == -8) face_card(f, "Встреча в 15:30, адрес: Большая Никитская, 24/1, стр. 5. Ссылка: "
        "https://meet.example.com/abc-defg-hij?pwd=Zx81Qk");
    if (ev <= -10) face_bubble(f, (bubble_icon_t)(-10 - ev), bub_text[-10 - ev], -1);
}
static void configure_tile(face_t *f, const tile_t *tile, const uint8_t *qr_join, int n_join,
                           const uint8_t *qr_page, int n_page, const char *const bub_text[BUB_COUNT]) {
    face_init(f); f->boot_t = 5;
    if (getenv("SIM_CHARACTER") && !strcmp(getenv("SIM_CHARACTER"), "tess")) face_set_character(f, CHARACTER_TESS);
    face_set_mode(f, tile->mode);
    if (tile->emo != EMO_NEUTRAL) face_set_emotion(f, (emotion_t)tile->emo, 100);
    if (tile->ev >= 0 && tile->ev < 100) face_event(f, (face_event_t)tile->ev, 0.6f, 0);
    configure_menu(f, tile->ev);
    if (tile->ev == 102) f->cron_running = 1; if (tile->ev == 103) f->cron_due = f->t + 12 * 60;
    configure_setup(f, tile->ev, qr_join, n_join, qr_page, n_page); configure_cards(f, tile->ev, bub_text);
}
static void animate_tile(face_t *f, const tile_t *tile) {
    for (int k = 0; k < 90; k++) {
        f->next_blink = 100; f->next_micro = 100;
        f->mic_level = tile->mic * (0.6f + 0.4f * sinf(k * 0.7f));
        f->spk_level = tile->spk;
        if (tile->ev == FEV_VOLUME) f->volume_show = 1;
        if (tile->ev == -9) f->slide_x = 0.4f, f->slide_y = 0.3f;
        if (tile->ev == -9 && k == 84) f->jolt_dvx = 0.02f;
        face_update(f, 1.f / R_FPS);
    }
}
static void compose_tile(face_t *f, uint16_t big[16 * PANEL_H][4 * PANEL_W], int i, int cols) {
    sim_render(f);
    int ox = (i % cols) * PANEL_W, oy = (i / cols) * PANEL_H;
    for (int y = 0; y < PANEL_H; y++) memcpy(&big[oy + y][ox], panel[y], PANEL_W * 2);
    for (int y = 0; y < PANEL_H; y++) big[oy + y][ox] = 0x4208;
    for (int x = 0; x < PANEL_W; x++) big[oy][ox + x] = 0x4208;
}
static void emit_sheet(uint16_t big[16 * PANEL_H][4 * PANEL_W], int cols, int rows, int n) {
    static unsigned char row[4 * PANEL_W * 3];
    for (int y = 0; y < rows * PANEL_H; y++) {
        for (int x = 0; x < cols * PANEL_W; x++) sim_to_rgb(big[y][x], row + x * 3);
        fwrite(row, 1, cols * PANEL_W * 3, stdout);
    }
    fprintf(stderr, "sheet %dx%d, %d tiles\n", cols * PANEL_W, rows * PANEL_H, n);
}
void sim_sheet(bool per_tile) {
    tile_t tiles[] = {
        {"neutral", MODE_IDLE, EMO_NEUTRAL, 0, 0, -1},     {"happy", MODE_IDLE, EMO_HAPPY, 0, 0, -1},
        {"joy", MODE_IDLE, EMO_JOY, 0, 0, -1},             {"love", MODE_IDLE, EMO_LOVE, 0, 0, -1},
        {"sad", MODE_IDLE, EMO_SAD, 0, 0, -1},             {"angry", MODE_IDLE, EMO_ANGRY, 0, 0, -1},
        {"surprised", MODE_IDLE, EMO_SURPRISED, 0, 0, -1}, {"confused", MODE_IDLE, EMO_CONFUSED, 0, 0, -1},
        {"sleepy", MODE_IDLE, EMO_SLEEPY, 0, 0, -1},       {"thinking", MODE_THINKING, EMO_NEUTRAL, 0, 0, -1},
        {"wink", MODE_IDLE, EMO_WINK, 0, 0, -1},           {"shy", MODE_IDLE, EMO_SHY, 0, 0, -1},
        {"proud", MODE_IDLE, EMO_PROUD, 0, 0, -1},         {"dizzy", MODE_IDLE, EMO_DIZZY, 0, 0, -1},
        {"listening", MODE_LISTENING, EMO_NEUTRAL, 0.7f, 0, -1},
        {"speaking", MODE_SPEAKING, EMO_HAPPY, 0, 0.6f, -1},
        {"sleep", MODE_SLEEP, EMO_NEUTRAL, 0, 0, -1},      {"setup", MODE_SETUP, EMO_NEUTRAL, 0, 0, -1},
        {"server disconnected", MODE_OFFLINE, EMO_NEUTRAL, 0, 0, -1},  {"volume", MODE_IDLE, EMO_HAPPY, 0, 0, FEV_VOLUME},
        {"setup: join Wi-Fi", MODE_SETUP, EMO_NEUTRAL, 0, 0, -2},       {"setup: open page", MODE_SETUP, EMO_NEUTRAL, 0, 0, -3},
        {"setup: connecting", MODE_SETUP, EMO_NEUTRAL, 0, 0, -4},
        {"pairing", MODE_SETUP, EMO_NEUTRAL, 0, 0, -5},
        {"bubble: not heard", MODE_IDLE, EMO_CONFUSED, 0, 0, -10 - BUB_NOT_HEARD},
        {"bubble: error", MODE_IDLE, EMO_SAD, 0, 0, -10 - BUB_ERROR},
        {"bubble: busy", MODE_THINKING, EMO_NEUTRAL, 0, 0, -10 - BUB_BUSY},
        {"bubble: no wifi", MODE_OFFLINE, EMO_NEUTRAL, 0, 0, -10 - BUB_NO_WIFI},
        {"bubble: no server", MODE_OFFLINE, EMO_NEUTRAL, 0, 0, -10 - BUB_NO_SERVER},
        {"bubble: talk", MODE_IDLE, EMO_HAPPY, 0, 0, -10 - BUB_TALK},
        {"bubble: key", MODE_OFFLINE, EMO_NEUTRAL, 0, 0, -10 - BUB_KEY},
        {"bubble: plugin", MODE_OFFLINE, EMO_NEUTRAL, 0, 0, -10 - BUB_PLUGIN},
        {"status: listening", MODE_LISTENING, EMO_NEUTRAL, 0.7f, 0, -10 - BUB_REC},
        {"status: thinking", MODE_THINKING, EMO_NEUTRAL, 0, 0, -10 - BUB_THINK},
        {"status: connected", MODE_IDLE, EMO_JOY, 0, 0, -10 - BUB_OK},
        {"status: volume", MODE_IDLE, EMO_HAPPY, 0, 0, -10 - BUB_VOLUME},
        {"status: stopped", MODE_IDLE, EMO_NEUTRAL, 0, 0, -10 - BUB_STOP},
        {"activity: tool", MODE_THINKING, EMO_NEUTRAL, 0, 0, -10 - BUB_TOOL},
        {"activity: coding", MODE_THINKING, EMO_NEUTRAL, 0, 0, -10 - BUB_CODE},
        {"activity: web", MODE_THINKING, EMO_NEUTRAL, 0, 0, -10 - BUB_WEB},
        {"activity: deploy", MODE_THINKING, EMO_NEUTRAL, 0, 0, -10 - BUB_DEPLOY},
        {"activity: build", MODE_THINKING, EMO_NEUTRAL, 0, 0, -10 - BUB_BUILD},
        {"activity: compacting", MODE_THINKING, EMO_NEUTRAL, 0, 0, -10 - BUB_COMPACT},
        {"activity: elsewhere", MODE_IDLE, EMO_NEUTRAL, 0, 0, -10 - BUB_WEB},
        {"power: turning off", MODE_SLEEP, EMO_SLEEPY, 0, 0, -10 - BUB_POWER},
        {"card: short", MODE_SPEAKING, EMO_HAPPY, 0, 0.5f, -6},
        {"card: pages", MODE_IDLE, EMO_NEUTRAL, 0, 0, -7},
        {"card: link", MODE_IDLE, EMO_NEUTRAL, 0, 0, -8},
        {"tilted: face slides", MODE_IDLE, EMO_NEUTRAL, 0, 0, -9},
        {"menu: settings", MODE_IDLE, EMO_NEUTRAL, 0, 0, 100},
        {"menu: power armed", MODE_IDLE, EMO_NEUTRAL, 0, 0, 104},
        {"menu: opening", MODE_IDLE, EMO_NEUTRAL, 0, 0, 105},
        {"cron: running", MODE_IDLE, EMO_NEUTRAL, 0, 0, 102},
        {"cron: reminder due", MODE_IDLE, EMO_NEUTRAL, 0, 0, 103},
    };
    static const char *const bub_text[BUB_COUNT] = {
        [BUB_NOT_HEARD] = "Didn\u2019t catch that", [BUB_ERROR] = "Try again", [BUB_NO_SERVER] = "Agent offline",
        [BUB_TALK] = "Hold the button to talk", [BUB_PLUGIN] = "Connect agent adapter",
        [BUB_REC] = "Press again to send", [BUB_THINK] = "Thinking", [BUB_OK] = "Connected",
        [BUB_VOLUME] = "Volume 60%", [BUB_STOP] = "Stopped", [BUB_TOOL] = "Using tools", [BUB_CODE] = "Coding",
        [BUB_WEB] = "Browsing", [BUB_DEPLOY] = "Deploying", [BUB_BUILD] = "Building", [BUB_COMPACT] = "Tidying memory", [BUB_POWER] = "Turning off"};
    static uint8_t qr_join[QR_MAX_N * QR_MAX_N], qr_page[QR_MAX_N * QR_MAX_N];
    int n_join = qr_make("WIFI:T:WPA;S:KUBIK-C634;P:48210937;;", qr_join);
    int n_page = qr_make("http://192.168.4.1/", qr_page);
    int n = sizeof(tiles) / sizeof(tiles[0]);
    // SIM_ONLY=<label prefix> renders just those tiles (e.g. "activity:").
    const char *only = getenv("SIM_ONLY");
    if (only) {
        int m = 0;
        for (int i = 0; i < n; i++)
            if (!strncmp(tiles[i].label, only, strlen(only))) tiles[m++] = tiles[i];
        n = m;
    }
    int cols = 4, rows = (n + cols - 1) / cols;
    static uint16_t big[16 * PANEL_H][4 * PANEL_W];
    memset(big, 0x21, sizeof(big));
    for (int i = 0; i < n; i++) {
        face_t f;
        configure_tile(&f, &tiles[i], qr_join, n_join, qr_page, n_page, bub_text);
        animate_tile(&f, &tiles[i]);
        if (tiles[i].ev == 105) f.menu.k = 0.62f;  // tiles popping in
        if (per_tile) {
            sim_render(&f);
            sim_emit_frame();
        } else compose_tile(&f, big, i, cols);
    }
    if (!per_tile) emit_sheet(big, cols, rows, n);
}
