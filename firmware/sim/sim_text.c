// Every screen with text, one settled frame each, for reviewing type: bubbles, the settings menu, the setup and
// pairing cards, a reading card, the battery and cron labels.
//   sim text > text.rgb      (480x480 rgb24 frames; the names go to stderr)
#include <stdio.h>
#include <string.h>

#include "sim.h"
#include "../main/qr.h"

static face_t fresh(void) {
    face_t f;
    face_init(&f);
    face_set_mode(&f, MODE_IDLE);
    f.body.req = -1;
    return f;
}

static void settle(face_t *f, int frames) {
    for (int i = 0; i < frames; i++) face_update(f, 1.f / R_FPS);
}

static void shot(face_t *f, const char *name) {
    sim_render_reset();
    sim_render(f);
    sim_emit_frame();
    fprintf(stderr, "%s\n", name);
}

static void bubbles(void) {
    static const struct { bubble_icon_t icon; const char *text; } k_bubbles[] = {
        {BUB_NOT_HEARD, "Didn\xE2\x80\x99t catch that"}, {BUB_NO_SERVER, "Agent offline"},
        {BUB_BUSY, "Waiting for network time"}, {BUB_TALK, "Hold the button to talk"},
        {BUB_PLUGIN, "Connect agent adapter"}, {BUB_THINK, "Thinking"}, {BUB_VOLUME, "Volume 60%"},
        {BUB_OK, "Paired"}, {BUB_COMPACT, "Tidying memory"}, {BUB_ERROR, "Server key changed"},
    };
    for (unsigned i = 0; i < sizeof k_bubbles / sizeof k_bubbles[0]; i++) {
        face_t f = fresh();
        face_bubble(&f, k_bubbles[i].icon, k_bubbles[i].text, 30);
        settle(&f, 20);
        shot(&f, k_bubbles[i].text);
    }
}

static void menu(bool pressed) {
    face_t f = fresh();
    face_menu_t *m = &f.menu;
    static const char *const words[MT_COUNT] = {"Wi-Fi", "Power off", "Reset"};
    memcpy(m->txt, words, sizeof words);
    m->volume = 60; m->brightness = 35;
    m->open = true;
    settle(&f, 40);
    if (pressed) {  // the level shows while the finger is on the slider
        m->pressed = MENU_VOLUME;
        m->pressed_t = 0;
        settle(&f, 3);
    }
    shot(&f, pressed ? "menu volume touched" : "menu");
}

static void setup(const char *title, const char *detail, const char *url, int step) {
    face_t f = fresh();
    uint8_t mods[QR_MAX_N * QR_MAX_N];
    int n = url ? qr_make(url, mods) : 0;
    face_set_mode(&f, MODE_SETUP);
    face_set_setup(&f, mods, n, step, title, detail);
    settle(&f, 24);
    shot(&f, title);
}

static void cards(void) {
    face_t f = fresh();
    face_set_mode(&f, MODE_SETUP);
    face_set_pairing(&f, "K7QX4M2R");
    settle(&f, 40);
    shot(&f, "pairing");
    f = fresh();
    face_card(&f, "Sure. The forecast for tomorrow is 18\xC2\xB0 and clear, with a light wind from the west in the evening.\n"
                  "\xD0\x97\xD0\xB0\xD0\xB2\xD1\x82\xD1\x80\xD0\xB0 \xD0\xB1\xD1\x83\xD0\xB4\xD0\xB5\xD1\x82 \xD1\x8F\xD1\x81\xD0\xBD\xD0\xBE, +18 \xD0\xB3\xD1\x80\xD0\xB0\xD0\xB4\xD1\x83\xD1\x81\xD0\xBE\xD0\xB2.");
    settle(&f, 24);
    shot(&f, "card");
    f = fresh();
    face_set_power(&f, true, 12, false, false);
    f.cron_due = 1e9f; f.cron_k = 1;
    f.cron_due = f.t + 500;
    settle(&f, 30);
    shot(&f, "battery low and cron countdown");
}

void sim_text(void) {
    bubbles();
    menu(false);
    menu(true);
    setup("Scan to join Wi-Fi", "Kubik-4F2A  \xC2\xB7  password kubik4f2a", "WIFI:T:WPA;S:Kubik-4F2A;P:kubik4f2a;;", 1);
    setup("Scan again for setup", "192.168.4.1", "http://192.168.4.1/", 2);
    setup("Connecting\xE2\x80\xA6", "\xE2\x80\x9CHome network\xE2\x80\x9D", NULL, 3);
    cards();
}
