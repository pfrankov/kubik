#include "app_lab.h"
#include "app_internal.h"
#include "audio.h"
#include "audio_sfx.h"
#include "screen_lab.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static screen_lab_t *s_lab;
static atomic_bool s_active;
void app_lab_game_snapshot(tess_games_t *games, bool *preview) {
    face_lock();
    *preview = s_lab != NULL;
    *games = s_lab ? s_lab->face.tess_games : g_face.tess_games;
    face_unlock();
}
bool app_lab_active(void) { return atomic_load(&s_active); }
const char *app_lab_screen(void) {
    face_lock();
    const char *name = s_lab ? screen_lab_name(s_lab->selected) : "";
    face_unlock(); return name;
}
static bool available(void) {
    return s_menu && !app_voice_live_active() && !talk_is_listening(s_talk) && s_srv == SS_IDLE && s_gen < 0 && !audio_stream_playing() && !s_act_own;
}
bool app_lab_open(void) {
    if (!available()) return false;
    screen_lab_t *lab = calloc(1, sizeof *lab);
    if (!lab) return false;
    screen_lab_init(lab, KUBIK_CHARACTER);
    face_lock();
    if (s_lab) { face_unlock(); free(lab); return false; }
    s_lab = lab;
    g_face.rub_in = (rub_input_t){0};
    atomic_store(&s_active, true);
    face_unlock();
    s_menu_touch_ms = now_ms();
    disp_wake();
    return true;
}
void app_lab_close(void) {
    face_lock();
    if (!s_lab) { face_unlock(); return; }
    screen_lab_t *lab = s_lab;
    s_lab = NULL;
    atomic_store(&s_active, false);
    g_face.rub_in = (rub_input_t){0};
    audio_sfx_quiet();  // same face -> SFX lock order as preview submission
    face_unlock();
    free(lab);
}
static bool route(const app_ev_t *e, lab_event_t *event) {
    switch (e->type) {
    case EV_TAP: *event = LAB_TAP; return true;
    case EV_BOOT_SHORT: *event = LAB_BOOT; return true;
    case EV_PTT_DOWN: *event = LAB_KEY; return true;
    case EV_MENU_HOLD: *event = LAB_HOLD; return true;
    default: return false;
    }
}
static bool passive(app_ev_type_t type) {
    switch (type) {
    case EV_TOUCH_DOWN: case EV_PTT_UP: case EV_SWIPE_UP:
    case EV_PET: case EV_SHAKE: case EV_PICKUP: return true;
    default: return false;
    }
}
static void preview_drag(const app_ev_t *e) {
    int x = e->a & 0xFFFF; bool first = e->a >> 16;
    if (device_status_drag(&s_lab->face.status, x, e->b, first) ||
        event_journal_drag(&s_lab->face.journal, x, e->b, first)) return;
    agent_menu_drag(&s_lab->face.agent, x, e->b, first);
}
bool app_lab_event(const app_ev_t *e) {
    if (!app_lab_active()) return false;
    if (e->type == EV_PWR_SHORT || e->type == EV_PWR_LONG || e->type == EV_BOOT_LONG || e->type == EV_SETUP_START) {
        app_lab_close(); return false;
    }
    if (e->type == EV_DRAG) {
        face_lock();
        preview_drag(e);
        face_unlock(); s_menu_touch_ms = now_ms(); return true;
    }
    lab_event_t event;
    if (route(e, &event)) {
        face_lock();
        bool leave = event == LAB_BOOT && s_lab->selected < 0;
        if (!leave) screen_lab_event(s_lab, event, e->a, e->b);
        face_unlock();
        if (leave) app_lab_close();
        s_menu_touch_ms = now_ms(); disp_wake();
        return true;
    }
    if (passive(e->type)) {
        s_menu_touch_ms = now_ms(); return true;
    }
    return false;
}
bool app_lab_frame(scene_t *scene, float dt, bool powered) {
    if (!s_lab) return false;
    face_t *f = &s_lab->face;
    f->dark = !powered;
    f->tilt_x = g_face.tilt_x; f->tilt_y = g_face.tilt_y;
    f->grav_x = g_face.grav_x; f->grav_y = g_face.grav_y;
    f->slide_x = g_face.slide_x; f->slide_y = g_face.slide_y;
    f->jolt_dvx = g_face.jolt_dvx; f->jolt_dvy = g_face.jolt_dvy;
    g_face.jolt_dvx = g_face.jolt_dvy = 0;
    memcpy(f->view_q, g_face.view_q, sizeof f->view_q);
    s_lab->face.rub_in = g_face.rub_in;
    g_face.rub_in.path = 0; g_face.rub_in.turns = 0;
    if (s_lab->face.menu.open && !s_lab->face.agent.open && !s_lab->face.journal.open)
        screen_lab_pointer(s_lab, s_lab->face.rub_in.down, s_lab->face.rub_in.x, s_lab->face.rub_in.y);
    screen_lab_update(s_lab, dt);
    if (powered) screen_lab_draw(s_lab, scene);
    return true;
}

bool app_lab_take_cue(tess_cue_t *cue, float *strength, float *position) {
    return s_lab && screen_lab_take_cue(s_lab, cue, strength, position);
}

void app_lab_play_cue(tess_cue_t cue, float strength, float position) {
    // Revalidate under the same lock as close: a copied frame cannot restart
    // preview sound after exit. The SFX mixer never takes the face lock.
    face_lock();
    if (s_lab && s_lab->selected == LAB_OFFLINE && !s_lab->face.dark && cue == TC_IMPACT)
        audio_tess_cue(cue, strength, position);
    face_unlock();
}
