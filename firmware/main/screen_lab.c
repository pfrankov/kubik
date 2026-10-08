#include "screen_lab.h"
#include "face_agent.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

bool screen_lab_controls(const screen_lab_t *lab) {
    return (lab->selected >= LAB_RECORD && lab->selected <= LAB_LIVE) || lab->selected == LAB_EVENTS;
}
int screen_lab_control_y(const screen_lab_t *lab) { return (lab->selected == LAB_LIVE || lab->selected == LAB_EVENTS) ? 92 : 428; }
static bool tess_debug_preview(int screen) { return screen >= LAB_GROW_POINT && screen <= LAB_CATCH_2; }
static void select_model(screen_lab_t *lab, face_agent_hit_t hit) {
    agent_menu_t *m = &lab->face.agent;
        unsigned index = hit - AGENT_HIT_MODEL_0;
        if (index >= m->count) return;
        strcpy(m->selected_model, m->models[index].id);
        lab->model_choice[m->target] = index;
        if (m->target == AGENT_TARGET_AGENT) strcpy(m->model, m->selected_model);
        if (m->target == AGENT_TARGET_MODE) {
            m->voice_mode = index;
            agent_menu_voice_models(m);
            screen_lab_catalog(lab);
        }
}
static bool visit(agent_menu_t *m, face_agent_hit_t hit) {
    switch (hit) {
    case AGENT_HIT_MODELS: agent_menu_visit(m, AGENT_VIEW_MODELS, AGENT_TARGET_AGENT); break;
    case AGENT_HIT_VOICE: agent_menu_visit(m, AGENT_VIEW_VOICE_MODES, AGENT_TARGET_MODE); break;
    case AGENT_HIT_STT: agent_menu_visit(m, AGENT_VIEW_MODELS, AGENT_TARGET_STT); break;
    case AGENT_HIT_TTS: agent_menu_visit(m, AGENT_VIEW_MODELS, AGENT_TARGET_TTS); break;
    default: return false;
    }
    return true;
}
static void agent_tap(screen_lab_t *lab, int x, int y) {
    agent_menu_t *m = &lab->face.agent;
    if (!agent_menu_take_tap(m)) return;
    face_agent_hit_t hit = face_agent_hit(m, x, y);
    if (visit(m, hit)) { screen_lab_catalog(lab); return; }
    if (hit == AGENT_HIT_GUIDE_NEXT) agent_menu_guide_move(m, 1);
    else if (hit == AGENT_HIT_GUIDE_DONE || hit == AGENT_HIT_GUIDE_SKIP) agent_menu_hide(m);
    else if (hit == AGENT_HIT_NEXT || hit == AGENT_HIT_PREV) {
        uint8_t cursor; agent_menu_turn_page(m, hit == AGENT_HIT_NEXT, &cursor);
    } else if (hit >= AGENT_HIT_MODEL_0 && hit <= AGENT_HIT_MODEL_3) {
        select_model(lab, hit);
        return;
    } else return;
    if (m->view != AGENT_VIEW_GUIDE) screen_lab_catalog(lab);
}
static bool menu_detail_open(screen_lab_t *lab, int row) {
    face_t *f = &lab->face;
    if (row == MENU_STATUS) {
        f->status = (device_status_t){.open=true, .connected=true, .radio=true, .agent=true,
            .ssid="Demo Wi-Fi", .ip="192.168.1.42", .gateway="192.168.1.1", .dns="192.168.1.1",
            .mac="02:00:00:12:34:56", .hostname="kubik-demo", .name="Demo Kubik", .firmware="Preview", .route="wifi", .url="wss://agent.example.com/kubik/v1"};
    }
    else if (row == MENU_EVENTS) event_journal_open(&f->journal);

    else return false;
    return true;
}
static void menu_tap(screen_lab_t *lab, int x, int y) {
    face_t *f = &lab->face;
    if (device_status_tap(&f->status, y)) return;
    if (f->journal.open) { event_journal_tap(&f->journal, x, y); return; }
    if (f->agent.open) { agent_tap(lab, x, y); return; }
    if (menu_service_tap(&lab->service, x, y, (int64_t)(f->t * 1000))) {
        f->menu.service = true; return;
    }
    int value, row = face_menu_hit(f, x, y, &value);
    if (menu_detail_open(lab, row)) return;
    else if (row == MENU_VOLUME) f->menu.volume = lab->volume = value;
    else if (row == MENU_UI_VOLUME) f->menu.ui_volume = lab->ui_volume = value;
    else if (row == MENU_BRIGHT) f->menu.brightness = lab->brightness = value;
    else if (row == MENU_AGENT) { agent_menu_show(&f->agent, true); screen_lab_catalog(lab); }
    else if (row == MENU_GUIDE) { agent_menu_start_guide(&f->agent, true); screen_lab_catalog(lab); f->agent.volume = lab->volume; }
    else if (row == MENU_ACTIONS) screen_lab_select(lab, value == 0 ? LAB_WIFI_QR : LAB_HOME);
}
static void catalog_tap(screen_lab_t *lab, int x, int y) {
    if (y >= 104 && y < 368 && x >= 20 && x <= 460) {
        int index = lab->page * 3 + (y - 104) / 88;
        if (index < LAB_COUNT) screen_lab_select(lab, index);
    } else if (y >= 396 && y <= 460) {
        int pages = (LAB_COUNT + 2) / 3;
        lab->page = (lab->page + (x < 240 ? pages - 1 : 1)) % pages;
    }
}
static void back(screen_lab_t *lab) {
    face_t *f = &lab->face;
    if (device_status_back(&f->status) || event_journal_back(&f->journal)) return;
    if (f->agent.open && f->agent.view == AGENT_VIEW_GUIDE) {
        if (f->agent.guide_step) agent_menu_guide_move(&f->agent, -1);
        else agent_menu_hide(&f->agent);
    } else if (f->agent.open && f->agent.view != AGENT_VIEW_OVERVIEW) {
        agent_menu_back(&f->agent); screen_lab_catalog(lab);
    } else if (f->agent.open) agent_menu_hide(&f->agent);
    else if (f->menu.sound) f->menu.sound = false;
    else screen_lab_select(lab, -1);
}
static void next_preview(screen_lab_t *lab) {
    if (lab->selected == LAB_EVENTS) screen_lab_demo_event(lab);
    else if (lab->selected == LAB_LIVE) {
        face_mode_t mode = lab->face.mode;
        face_set_mode(&lab->face, mode == MODE_LISTENING ? MODE_THINKING : mode == MODE_THINKING ? MODE_SPEAKING : MODE_LISTENING);
    } else screen_lab_select(lab, lab->selected + 1);
}
static void tap(screen_lab_t *lab, int x, int y) {
    if (lab->selected < 0) { catalog_tap(lab, x, y); return; }
    if (screen_lab_controls(lab) && lab->overlay && abs(y - screen_lab_control_y(lab)) <= 32) {
        if (x < 160) lab->signal = !lab->signal;
        else if (x < 320) next_preview(lab);
        else screen_lab_select(lab, -1);
    } else if (lab->face.menu.open) menu_tap(lab, x, y);
    else if (!face_card_tap(&lab->face) && (lab->selected == LAB_HOME || tess_debug_preview(lab->selected)))
        face_event(&lab->face, FEV_TAP, x, y);
}
static void key(screen_lab_t *lab) {
    if (lab->selected == LAB_EVENTS) screen_lab_demo_event(lab);
    else if (lab->selected >= LAB_CONNECT && lab->selected < LAB_PAIR) screen_lab_select(lab, lab->selected + 1);
    else if (lab->selected == LAB_LIVE) screen_lab_select(lab, LAB_HOME);
    else if (tess_debug_preview(lab->selected)) screen_lab_select(lab, lab->selected);
    else if (screen_lab_controls(lab)) lab->signal = !lab->signal;
    else screen_lab_select(lab, LAB_RECORD);
}
void screen_lab_event(screen_lab_t *lab, lab_event_t event, int x, int y) {
    switch (event) {
    case LAB_TAP: tap(lab, x, y); break;
    case LAB_BOOT: back(lab); break;
    case LAB_MENU: screen_lab_select(lab, LAB_SETTINGS); break;
    case LAB_POWER: lab->face.dark = !lab->face.dark; break;
    case LAB_KEY: key(lab); break;
    case LAB_HOLD: {
        if (screen_lab_controls(lab)) { lab->overlay = !lab->overlay; break; }
        int part;
        if (!lab->face.agent.open && face_menu_hit(&lab->face, x, y, &part) == MENU_VOLUME) lab->face.menu.sound = true;
        break;
    }
    }
}
static bool menu_sliders(const face_t *f) {
    return f->menu.open && !f->agent.open && !f->journal.open && !f->status.open;
}
static bool detail_drag(face_t *f, int x, int y, bool first) {
    return device_status_drag(&f->status, x, y, first) || event_journal_drag(&f->journal, x, y, first);
}
void screen_lab_pointer(screen_lab_t *lab, bool down, int x, int y) {
    bool first = !lab->finger.active;
    bool moved = lab->finger.active && (fabsf(lab->finger.last_x - x) + fabsf(lab->finger.last_y - y) >= 4);
    rub_touch(&lab->finger, &lab->face.rub_in, down, x, y);
    if (down && detail_drag(&lab->face, x, y, first)) return;
    if (down && lab->face.agent.open) agent_menu_drag(&lab->face.agent, x, y, first);
    if (down && moved && menu_sliders(&lab->face)) {
        int value, row = face_menu_hit(&lab->face, x, y, &value);
        if (row == MENU_VOLUME || row == MENU_UI_VOLUME || row == MENU_BRIGHT) menu_tap(lab, x, y);
    }
}
void screen_lab_update(screen_lab_t *lab, float dt) {
    face_t *f = &lab->face;
    dt = fminf(0.1f, fmaxf(0.001f, dt));
    float level = lab->signal ? 0.15f + 0.65f * fabsf(sinf(f->t * 7.3f)) : 0;
    f->mic_level = f->mode == MODE_LISTENING ? level : 0;
    f->spk_level = f->mode == MODE_SPEAKING ? level : 0;
    face_update(f, dt);
    f->rub_in.path = 0; f->rub_in.turns = 0;
    if (lab->selected != LAB_OFFLINE && lab->selected < LAB_GROW_POINT) {
        tess_cue_t cue; float strength, position;
        while (face_take_cue(f, &cue, &strength, &position)) {}
    }
}

// Preview only local character feedback, never provider speech or capture.
bool screen_lab_cue_allowed(const screen_lab_t *lab, tess_cue_t cue) {
    if (lab->face.dark) return false;
    if (lab->selected == LAB_OFFLINE) return cue == TC_IMPACT;
    if (lab->selected < LAB_GROW_POINT || lab->selected >= LAB_COUNT) return false;
    return cue == TC_TOUCH || cue == TC_FLING || cue == TC_SWING || cue == TC_EXCITE ||
        cue == TC_DODGE || cue == TC_JOY;
}
bool screen_lab_take_cue(screen_lab_t *lab, tess_cue_t *cue, float *strength, float *position) {
    while (face_take_cue(&lab->face, cue, strength, position))
        if (screen_lab_cue_allowed(lab, *cue)) return true;
    return false;
}
