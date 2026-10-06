#include "screen_lab.h"
#include "app_lab.h"
#include "app_status.h"
#include "app_internal.h"

#include "audio.h"
#include "board.h"
#include "esp_log.h"
#include "input.h"
#include "menu_service.h"
#include "face_agent.h"
#include "cJSON.h"
#include "ui_text.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app";
static menu_service_t s_service;
static screen_lab_unlock_t s_lab_unlock;

// ------------------------------------------------------------------ settings menu
static int bright_pct(void) { return (g_settings.brightness - 10) * 100 / 245; }

// The menu as the face draws it.
void menu_sync(void) {
    static const str_id_t words[MT_COUNT] = {STR_WIFI, STR_POWER_OFF, STR_RESET};
    face_lock();
    agent_menu_set_online(&g_face.agent, s_online);
    g_face.agent.volume = (uint8_t)g_settings.volume;
    face_menu_t *m = &g_face.menu;
    for (int i = 0; i < MT_COUNT; i++) m->txt[i] = str(words[i]);
    g_face.journal.overlay = g_settings.event_overlay;
    m->open = s_menu;
    m->service = s_menu && s_service.unlocked;
    m->armed = s_armed_ms ? s_armed_part + 1 : 0;
    m->volume = g_settings.volume;
    m->ui_volume = g_settings.ui_volume;
    m->brightness = bright_pct();
    face_unlock();
}

static bool send_agent_request(const char *type, uint16_t rid, uint8_t cursor, const char *id) {
    cJSON *j = cJSON_CreateObject();
    if (!j) return false;
    bool ok = cJSON_AddStringToObject(j, "t", type) && cJSON_AddNumberToObject(j, "rid", rid) &&
        cJSON_AddNumberToObject(j, "cursor", cursor) &&
        cJSON_AddStringToObject(j, "target", agent_target_name(g_face.agent.target));
    if (ok && id) ok = cJSON_AddStringToObject(j, "id", id);
    char *json = ok ? cJSON_PrintUnformatted(j) : NULL;
    if (json) send_json("%s", json);
    cJSON_free(json);
    cJSON_Delete(j);
    return json != NULL;
}

static void request_agent_options(uint8_t cursor) {
    face_lock();
    uint16_t rid = agent_menu_request_options(&g_face.agent, cursor, (uint32_t)now_ms());
    face_unlock();
    if (rid && send_agent_request("agent_options", rid, cursor, NULL)) disp_wake();
}

static void request_agent_model(const char *id) {
    face_lock();
    uint8_t cursor = g_face.agent.cursor;
    uint16_t rid = agent_menu_request_model(&g_face.agent, id, (uint32_t)now_ms());
    face_unlock();
    if (rid && send_agent_request("agent_model", rid, cursor, id)) disp_wake();
}

static void request_agent_retry(void) {
    char id[AGENT_MODEL_ID_MAX + 1];
    uint8_t cursor;
    face_lock();
    bool selecting = g_face.agent.retry_kind == AGENT_REQUEST_SELECT;
    snprintf(id, sizeof id, "%s", selecting ? g_face.agent.retry_id : "");
    cursor = g_face.agent.retry_cursor;
    uint16_t rid = agent_menu_retry(&g_face.agent, (uint32_t)now_ms());
    face_unlock();
    if (rid && send_agent_request(selecting ? "agent_model" : "agent_options", rid, cursor, selecting ? id : NULL))
        disp_wake();
}

// Called with the face lock held; only crossing a wire page requests data.
static bool agent_screen_navigation(face_agent_hit_t hit, uint8_t *cursor) {
    agent_menu_t *m = &g_face.agent;
    switch (hit) {
    case AGENT_HIT_BACK: agent_menu_back(m); break;
    case AGENT_HIT_MODELS: agent_menu_visit(m, AGENT_VIEW_MODELS, AGENT_TARGET_AGENT); break;
    case AGENT_HIT_VOICE: agent_menu_visit(m, AGENT_VIEW_VOICE_MODES, AGENT_TARGET_MODE); break;
    case AGENT_HIT_STT: agent_menu_visit(m, AGENT_VIEW_MODELS, AGENT_TARGET_STT); break;
    case AGENT_HIT_TTS: agent_menu_visit(m, AGENT_VIEW_MODELS, AGENT_TARGET_TTS); break;
    case AGENT_HIT_PREV:
    case AGENT_HIT_NEXT: return agent_menu_turn_page(m, hit == AGENT_HIT_NEXT, cursor);
    default: return false;
    }
    *cursor = 0;
    return m->open;
}

void menu_back(void) {
    s_menu_touch_ms = now_ms();
    face_lock();
    if (device_status_back(&g_face.status) || event_journal_back(&g_face.journal)) {
        face_unlock(); audio_sfx(SFX_PAGE); disp_wake(); return;
    }
    bool open = g_face.agent.open;
    bool guide = open && g_face.agent.view == AGENT_VIEW_GUIDE;
    uint8_t cursor = g_face.agent.cursor;
    bool refresh = open && !guide && agent_screen_navigation(AGENT_HIT_BACK, &cursor);
    face_unlock();
    if (!open && g_face.menu.sound) {
        face_lock(); g_face.menu.sound = false; face_unlock();
        s_menu_drag_row = -1; audio_sfx(SFX_PAGE); menu_sync(); disp_wake(); return;
    }
    if (!open) { menu_close(true); return; }
    if (guide) app_guide_tap(AGENT_HIT_BACK);
    else if (refresh) request_agent_options(cursor);
    audio_sfx(SFX_PAGE);
    menu_sync();
    disp_wake();
}

static bool agent_selection(face_agent_hit_t hit, char *id, size_t cap) {
    agent_menu_t *m = &g_face.agent;
    if (hit < AGENT_HIT_MODEL_0 || hit > AGENT_HIT_MODEL_3) return false;
    unsigned i = (unsigned)(hit - AGENT_HIT_MODEL_0);
    if (i >= m->count) return false;
    if (m->view == AGENT_VIEW_VOICE_MODES && agent_menu_current(m, i)) {
        agent_menu_voice_models(m); return true;
    }
    snprintf(id, cap, "%s", m->models[i].id);
    return false;
}

static void agent_screen_tap(int x, int y) {
    char id[AGENT_MODEL_ID_MAX + 1] = "";
    face_lock();
    if (!agent_menu_take_tap(&g_face.agent)) { face_unlock(); return; }
    face_agent_hit_t hit = face_agent_hit(&g_face.agent, x, y);
    g_face.agent.pressed = (int8_t)hit;
    g_face.agent.pressed_t = 0;
    uint8_t cursor = g_face.agent.cursor;
    bool guide = g_face.agent.view == AGENT_VIEW_GUIDE;
    bool request_page = !guide && agent_screen_navigation(hit, &cursor);
    if (agent_selection(hit, id, sizeof id)) { request_page = true; cursor = 0; }
    face_unlock();
    if (guide && hit != AGENT_HIT_REFRESH) {
        app_guide_tap(hit); disp_wake(); return;
    }
    if (hit == AGENT_HIT_BACK) {
        audio_sfx(SFX_PAGE);
        menu_sync();
    } else if (request_page || hit == AGENT_HIT_REFRESH) request_agent_options(cursor);
    else if (id[0]) request_agent_model(id);
    else if (hit == AGENT_HIT_RETRY) request_agent_retry();
    disp_wake();
}

bool app_agent_receive_reply(const agent_menu_reply_t *reply) {
    face_lock();
    bool selecting_mode = g_face.agent.view == AGENT_VIEW_VOICE_MODES && g_face.agent.waiting_kind == AGENT_REQUEST_SELECT;
    bool accepted = agent_menu_accept_reply(&g_face.agent, reply, (uint32_t)now_ms());
    bool open = g_face.agent.open;
    bool open_models = accepted && open && selecting_mode && !g_face.agent.error[0];
    if (open_models) agent_menu_voice_models(&g_face.agent);
    face_unlock();
    if (accepted && open) disp_wake();
    return accepted;
}

void app_agent_set_capabilities(bool stt_available, bool tts_available) {
    if (!stt_available) { app_wake_abort(true); app_wake_suspend(); }
    face_lock();
    agent_menu_set_capabilities(&g_face.agent, stt_available, tts_available);
    g_face.agent.voice_mode = app_voice_mode();
    face_unlock();
    disp_wake();
}

void app_agent_refresh_if_open(void) {
    face_lock();
    bool open = g_face.agent.open;
    uint8_t cursor = g_face.agent.cursor;
    face_unlock();
    if (open) request_agent_options(cursor);
}

bool app_agent_mic_allowed(void) {
    face_lock();
    bool allowed = g_face.agent.capabilities_known && g_face.agent.stt_available;
    face_unlock();
    return allowed;
}

void app_agent_tick(int64_t now) {
    face_lock();
    bool timed_out = agent_menu_timeout(&g_face.agent, (uint32_t)now);
    bool open = g_face.agent.open;
    bool load = open && g_face.agent.online && g_face.agent.view != AGENT_VIEW_GUIDE &&
        !g_face.agent.options_loaded && !g_face.agent.request_pending && !g_face.agent.error[0];
    if (g_face.agent.pressed_t < 1.f) {
        g_face.agent.pressed_t += 0.05f;
        if (g_face.agent.pressed_t >= 0.24f) g_face.agent.pressed = -1;
    }
    face_unlock();
    // Reply callbacks run on the transport task; send follow-up requests here,
    // after releasing its WebSocket receive lock.
    if (load) request_agent_options(0);
    if (timed_out && open) disp_wake();
}

void device_state_volume_report(bool force) {
    static int reported = -1;
    int volume = g_settings.volume < 0 ? 0 : g_settings.volume > 100 ? 100 : g_settings.volume;
    if (!s_online || (!force && reported == volume)) return;
    send_json("{\"t\":\"device_state\",\"volume\":%d}", volume);
    reported = volume;
}

static void menu_press(int row, int part) {
    face_lock();
    g_face.menu.pressed = row;
    g_face.menu.pressed_part = part;
    g_face.menu.pressed_t = 0;
    face_unlock();
}

void menu_open(void) {
    app_voice_live_stop(true);
    app_wake_abort(true);
    app_wake_suspend();
    if (s_menu || s_setup || (s_pair_code[0] && !s_pair_hidden) || talk_is_listening(s_talk) || s_power_off_at) return;
    ESP_LOGI(TAG, "settings menu open");
    wake(false);
    s_menu = true;
    face_lock(); g_face.menu.sound = false; face_unlock();
    s_service = (menu_service_t){0};
    s_menu_touch_ms = now_ms();
    s_menu_drag_row = -1;
    menu_press(-1, 0);
    input_set_menu(true);
    audio_sfx(SFX_MENU_OPEN);
    menu_sync();
}

// `sound`: the menu's own close sound; false when what closes it has one (talking, power off, setup).
void menu_close(bool sound) {
    app_lab_close();
    s_lab_unlock = (screen_lab_unlock_t){0};
    if (!s_menu) return;
    if (sound) audio_sfx(SFX_MENU_CLOSE);
    ESP_LOGI(TAG, "settings menu closed");
    s_menu = false;
    face_lock();
    agent_menu_hide(&g_face.agent);
    g_face.menu.sound = false;
    g_face.status.open = false;
    g_face.journal.open = g_face.journal.detail = false;
    face_unlock();
    s_armed_ms = 0;
    input_set_menu(false);
    if (s_menu_dirty) settings_save();
    s_menu_dirty = false;
    menu_sync();
}

static void menu_slider(int row, int v) {
    if (row == MENU_VOLUME) {
        if (v == g_settings.volume) return;
        g_settings.volume = v;
        audio_set_volume(v);
        device_state_volume_report(false);
        if (v / 10 != s_menu_tick_level) {  // a wooden notch per 10 %, at the new loudness
            s_menu_tick_level = v / 10;
            audio_sfx_level(SFX_DETENT, v / 10);
        }
    } else if (row == MENU_UI_VOLUME) {
        if (v == g_settings.ui_volume) return;
        g_settings.ui_volume = v;
        audio_set_ui_volume(v);
        if (v / 10 != s_menu_tick_level) {
            s_menu_tick_level = v / 10; audio_sfx_level(SFX_DETENT, v / 10);
        }
    } else {
        int level = 10 + v * 245 / 100;
        if (level == g_settings.brightness) return;
        if (v / 10 != s_menu_tick_level) {  // a glassy notch per 10 %
            s_menu_tick_level = v / 10;
            audio_sfx_level(SFX_GLINT, v / 10);
        }
        g_settings.brightness = level;
        disp_brightness_fade(level, 0);
    }
    s_menu_dirty = true;
}

// Wi-Fi (0) acts at once; reset (1) and power off (2) need a second tap on the same tile within 3 s.
static bool menu_tap_actions(int part) {
    if (part == 1 && !s_service.unlocked) return false;
    if (part > 0 && (!s_armed_ms || s_armed_part != part)) {
        s_armed_ms = now_ms();
        s_armed_part = part;
        audio_sfx(SFX_ARM);
        return false;
    }
    if (part == 1 && factory_reset() != ESP_OK) {
        s_armed_ms = 0;
        audio_sfx(SFX_DENY);
        return false;
    }
    s_menu_dirty = false;  // a reset must not be undone by the menu's own save
    menu_close(false);
    if (part == 0) setup_enter(SETUP_BY_USER);
    else if (part == 2) power_off_begin();
    return true;
}

static bool menu_tap_agent(int row) {
    if (row == MENU_STATUS) {
        app_status_open(); s_menu_drag_row = -1; audio_sfx(SFX_PAGE); disp_wake(); return true;
    }
    if (row == MENU_EVENTS) {
        face_lock(); event_journal_open(&g_face.journal); face_unlock();
        s_menu_drag_row = -1; audio_sfx(SFX_PAGE); disp_wake(); return true;
    }
    if (row == MENU_GUIDE) { app_guide_start(); return true; }
    if (row != MENU_AGENT) return false;
    face_lock(); agent_menu_show(&g_face.agent, s_online); face_unlock();
    audio_sfx(SFX_PAGE);
    menu_sync();
    if (s_online) request_agent_options(0);
    return true;
}

static void lab_unlock_tap(int x, int y) {
    if (x < 218 && !g_face.agent.open && !g_face.menu.sound) screen_lab_unlock_tap(&s_lab_unlock, x, y, now_ms());
}
static bool journal_tap(int x, int y) {
    face_lock();
    bool events = g_face.journal.open;
    bool changed = events && event_journal_tap(&g_face.journal, x, y);
    if (changed) g_settings.event_overlay = g_face.journal.overlay;
    face_unlock();
    if (events) {
        if (changed) { settings_save(); audio_sfx(SFX_PAGE); }
        disp_wake(); return true;
    }
    return false;
}
static bool service_tap(int x, int y) {
    if (g_face.menu.k < 1.f || !menu_service_tap(&s_service, x, y, now_ms())) return false;
    audio_sfx(SFX_ARM); menu_sync(); return true;
}
static bool menu_detail_tap(int x, int y) {
    face_lock(); bool status = device_status_tap(&g_face.status, y); face_unlock();
    if (status) { disp_wake(); return true; }
    return journal_tap(x, y);
}
void menu_tap(int x, int y) {
    s_menu_touch_ms = now_ms();
    if (menu_detail_tap(x, y)) return;
    lab_unlock_tap(x, y);
    int part;
    if (g_face.agent.open) {
        s_menu_touch_ms = now_ms();
        agent_screen_tap(x, y);
        return;
    }
    if (service_tap(x, y)) return;
    int row = face_menu_hit(&g_face, x, y, &part);
    s_menu_touch_ms = now_ms();
    if (row < 0) return;
    menu_press(row, part);
    if (menu_tap_agent(row)) return;
    switch (row) {
    case MENU_VOLUME:
    case MENU_BRIGHT:
    case MENU_UI_VOLUME:
        s_menu_tick_level = -1;
        menu_slider(row, part);
        break;
    case MENU_ACTIONS:
        if (menu_tap_actions(part)) return;
        break;
    }
    if (row != MENU_ACTIONS && s_armed_ms) s_armed_ms = 0;
    menu_sync();
}

void menu_drag(int x, int y, bool first) {
    s_menu_touch_ms = now_ms();
    face_lock();
    if (device_status_drag(&g_face.status, x, y, first) || event_journal_drag(&g_face.journal, x, y, first)) {
        face_unlock(); s_menu_drag_row = -1; disp_wake(); return;
    }
    bool agent = agent_menu_drag(&g_face.agent, x, y, first);
    face_unlock();
    if (agent) { s_menu_drag_row = -1; return; }
    if (first) {
        int part, row = face_menu_hit(&g_face, x, y, &part);
        s_menu_drag_row = row == MENU_VOLUME || row == MENU_BRIGHT || row == MENU_UI_VOLUME ? row : -1;
        s_menu_tick_level = (row == MENU_BRIGHT ? bright_pct() : row == MENU_UI_VOLUME ? g_settings.ui_volume : g_settings.volume) / 10;  // notches from here on
        if (s_menu_drag_row >= 0) menu_press(row, part);
        menu_sync(); return; // A stationary hold must not change the level.
    }
    if (s_menu_drag_row < 0) return;
    menu_slider(s_menu_drag_row, face_menu_slider(y));
    menu_press(s_menu_drag_row, 0);
    menu_sync();
}

void menu_hold(int x, int y) {
    if (g_face.journal.open || g_face.status.open) return;
    if (s_menu && !g_face.agent.open && !g_face.menu.sound && screen_lab_entry(s_lab_unlock.ready, x, y)) {
        app_lab_open(); return;
    }
    if (!s_menu || g_face.agent.open || g_face.menu.sound || s_menu_drag_row != MENU_VOLUME) return;
    int part;
    if (face_menu_hit(&g_face, x, y, &part) != MENU_VOLUME) return;
    face_lock(); g_face.menu.sound = true; face_unlock();
    s_menu_drag_row = -1;
    s_menu_touch_ms = now_ms();
    audio_sfx(SFX_PAGE);
    menu_sync(); disp_wake();
}
