#include "screen_lab.h"
#include "tess.h"
#include "ui_text.h"
#include <string.h>
#include <stdio.h>

static const char *const names[LAB_COUNT] = {
    "Home", "Connecting", "Wi-Fi QR", "Setup QR", "Pairing", "Settings", "Sound",
    "Agent", "Voice modes", "Models", "Guide", "Recording", "Thinking", "Speaking",
    "GPT Live", "Text reply", "Offline points", "Error", "Background work", "Reminder", "Agent events", "Event log",
    "Growth: point", "Growth: square", "Growth: cube", "Growth: tesseract",
    "Duet: tier 1", "Duet: tier 2", "Duet: tier 3", "Chase: tier 1", "Chase: tier 2", "Chase: tier 3"
};
const char *screen_lab_name(int screen) { return screen >= 0 && screen < LAB_COUNT ? names[screen] : "Screen Lab"; }
bool screen_lab_unlock_tap(screen_lab_unlock_t *s, int x, int y, int64_t now) {
    if (!screen_lab_entry(true, x, y)) { s->taps = 0; s->ready = false; return false; }
    if (!s->taps || now - s->first_ms > 4000) { s->first_ms = now; s->taps = 0; }
    s->ready = ++s->taps >= 5;
    return s->ready;
}
bool screen_lab_entry(bool unlocked, int x, int y) { return unlocked && x >= 20 && x <= 260 && y >= 12 && y <= 56; }

static bool growth_preview(int screen) { return screen >= LAB_GROW_POINT && screen <= LAB_GROW_TESSERACT; }
static bool game_preview(int screen) { return screen >= LAB_ECHO_0 && screen <= LAB_CATCH_2; }
static bool tess_preview_only(screen_lab_t *lab) {
    if (face_character(&lab->face) == CHARACTER_TESS) return true;
    face_card(&lab->face, "Tess game preview only. Use a Tess build to inspect this screen.");
    return false;
}

static void apply_growth_preview(screen_lab_t *lab, int screen) {
    static const uint8_t progress[] = {0, 1, 7, TESS_PROGRESS_MASK};
    if (!tess_preview_only(lab)) return;
    tess_games_restore(&lab->face, progress[screen - LAB_GROW_POINT]);
    tess_games_set_available(&lab->face, true);
}

static void apply_game_preview(screen_lab_t *lab, int screen) {
    if (!tess_preview_only(lab)) return;
    unsigned tier;
    tess_game_t game;
    if (screen <= LAB_ECHO_2) {
        game = TESS_GAME_ECHO;
        tier = (unsigned)(screen - LAB_ECHO_0);
    } else {
        game = TESS_GAME_CATCH;
        tier = (unsigned)(screen - LAB_CATCH_0);
    }
    tess_games_restore(&lab->face, TESS_PROGRESS_MASK);
    tess_games_set_available(&lab->face, true);
    if (!tess_games_start(&lab->face, game, tier))
        face_card(&lab->face, "Could not start this Tess game preview.");
}

static void catalog(agent_menu_t *m) {
    m->online = m->options_loaded = m->capabilities_known = m->stt_available = m->tts_available = true;
    m->count = m->total = 3;
    const char *labels[] = {"Fast", "Detailed", "Balanced"};
    for (int i = 0; i < 3; i++) {
        snprintf(m->models[i].id, sizeof m->models[i].id, "demo/model-%d", i);
        snprintf(m->models[i].label, sizeof m->models[i].label, "%s", labels[i]);
    }
    if (!m->model[0]) strcpy(m->model, m->models[0].id);
    strcpy(m->selected_model, m->model);
    strcpy(m->stt_provider, "Demo"); strcpy(m->tts_provider, "Demo");
    strcpy(m->stt_model, "Recognition"); strcpy(m->tts_model, "Speech");
}
void screen_lab_catalog(screen_lab_t *lab) {
    agent_menu_t *m = &lab->face.agent;
    catalog(m);
    m->voice_mode = lab->model_choice[AGENT_TARGET_MODE];
    strcpy(m->model, m->models[lab->model_choice[AGENT_TARGET_AGENT]].id);
    strcpy(m->stt_model, m->models[lab->model_choice[AGENT_TARGET_STT]].label);
    strcpy(m->tts_model, m->models[lab->model_choice[AGENT_TARGET_TTS]].label);
    strcpy(m->selected_model, m->models[lab->model_choice[m->target]].id);
    if (m->view != AGENT_VIEW_VOICE_MODES) return;
    const char *ids[] = {"classic", "realtime", "live"};
    const char *labels[] = {"STT", "Realtime", "GPT Live"};
    for (int i = 0; i < 3; i++) {
        strcpy(m->models[i].id, ids[i]); strcpy(m->models[i].label, labels[i]);
    }
    strcpy(m->selected_model, ids[m->voice_mode]);
}
static void settings(face_t *f, const screen_lab_t *lab) {
    f->menu.open = true;
    f->menu.k = 1;
    f->menu.volume = lab->volume; f->menu.ui_volume = lab->ui_volume; f->menu.brightness = lab->brightness;
    f->menu.txt[MT_WIFI] = str(STR_WIFI); f->menu.txt[MT_POWER] = str(STR_POWER_OFF); f->menu.txt[MT_RESET] = str(STR_RESET);
}
static void setup(screen_lab_t *lab, int screen) {
    int n = 0, step = screen == LAB_SETUP_QR ? 2 : 1;
    const char *title = "Connecting", *detail = "Preview only";
    if (screen == LAB_WIFI_QR) {
        n = qr_make("WIFI:T:WPA;S:Kubik-DEMO;P:preview-only;;", lab->qr);
        title = "Join Kubik Wi-Fi"; detail = "Kubik-DEMO";
    } else if (screen == LAB_SETUP_QR) {
        n = qr_make("https://example.invalid/kubik-preview", lab->qr);
        title = "Scan again for setup";
    }
    face_set_mode(&lab->face, MODE_SETUP);
    face_set_setup(&lab->face, n ? lab->qr : NULL, n, step, title, detail);
    lab->face.qr_t = 1;
}
static void menu_screen(screen_lab_t *lab, int screen) {
    face_t *f = &lab->face;
        settings(f, lab);
        if (screen == LAB_SOUND) f->menu.sound = true;
        if (screen >= LAB_AGENT) {
            agent_menu_show(&f->agent, true); screen_lab_catalog(lab);
            if (screen == LAB_VOICE) { agent_menu_visit(&f->agent, AGENT_VIEW_VOICE_MODES, AGENT_TARGET_MODE); screen_lab_catalog(lab); }
            if (screen == LAB_MODELS) { agent_menu_visit(&f->agent, AGENT_VIEW_MODELS, AGENT_TARGET_AGENT); screen_lab_catalog(lab); }
            if (screen == LAB_GUIDE) { agent_menu_start_guide(&f->agent, true); screen_lab_catalog(lab); f->agent.volume = lab->volume; }
        }
}
static void character_screen(screen_lab_t *lab, int screen) {
    face_t *f = &lab->face;
    if (screen == LAB_RECORD) face_set_mode(f, MODE_LISTENING);
    else if (screen == LAB_THINK) face_set_mode(f, MODE_THINKING);
    else if (screen == LAB_SPEAK) face_set_mode(f, MODE_SPEAKING);
    else if (screen == LAB_LIVE) { f->live_active = f->live_ready = f->live_mic = true; face_set_mode(f, MODE_LISTENING); }
    else if (screen == LAB_TEXT) face_card(f, "This is a preview reply. Tap to turn the page. Long replies wrap and paginate with the same renderer as the device.\n\nNothing is sent to an agent and no settings are saved. You can inspect the layout, return with BOOT, and select another screen.");
    else if (screen == LAB_OFFLINE) face_set_mode(f, MODE_OFFLINE);
    else if (growth_preview(screen)) apply_growth_preview(lab, screen);
    else if (game_preview(screen)) apply_game_preview(lab, screen);
    else if (screen >= LAB_BACKGROUND) screen_lab_event_fixture(lab, screen);
    else if (screen == LAB_ERROR) face_bubble(f, BUB_ERROR, "Try again", 3600);
}
static void apply_screen(screen_lab_t *lab, int screen) {
    face_t *f = &lab->face;
    if (screen >= LAB_CONNECT && screen <= LAB_SETUP_QR) setup(lab, screen);
    else if (screen == LAB_PAIR) { face_set_mode(f, MODE_SETUP); face_set_pairing(f, "TEST2345"); f->qr_t = 1; }
    else if (screen >= LAB_SETTINGS && screen <= LAB_GUIDE) menu_screen(lab, screen);
    else character_screen(lab, screen);
}

void screen_lab_init(screen_lab_t *lab, int character) {
    memset(lab, 0, sizeof *lab);
    lab->character = character;
    lab->selected = -1;
    lab->volume = 60; lab->ui_volume = 50; lab->brightness = 75;
    screen_lab_select(lab, -1);
}
void screen_lab_select(screen_lab_t *lab, int screen) {
    if (screen < -1 || screen >= LAB_COUNT) return;
    lab->service = (menu_service_t){0};
    lab->selected = screen; lab->signal = true; lab->overlay = true;
    lab->finger = (rub_track_t){0};
    face_t *f = &lab->face;
    face_init(f);
#ifndef ESP_PLATFORM
    face_set_character(f, lab->character);
#endif
    face_set_mode(f, MODE_IDLE);
    face_set_power(f, true, 76, false, true);
    if (screen == LAB_EVENT_LOG) settings(f, lab);
    apply_screen(lab, screen);
}
