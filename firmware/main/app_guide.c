#include "app_internal.h"
#include "face_agent.h"
#include "esp_log.h"

static bool s_offered;

void app_guide_start(void) {
    menu_open();
    if (!s_menu) return;
    s_offered = true;
    face_lock();
    agent_menu_start_guide(&g_face.agent, s_online);
    g_face.agent.volume = (uint8_t)g_settings.volume;
    // The tour replaces the old unconditional "hold to talk" hint.
    g_face.bub_icon = BUB_NONE;
    face_unlock();
    s_next_bubble.icon = BUB_NONE;
    s_menu_touch_ms = now_ms();
    disp_wake();
}

static bool available(void) {
    return !s_menu && !s_setup && !s_pair_code[0] && s_talk == TALK_IDLE &&
        s_srv == SS_IDLE && s_gen < 0 && !s_act_own && !s_power_off_at;
}
void app_guide_tick(void) {
    if (s_offered || g_settings.guide_done || !s_online || !available()) return;
    face_lock();
    bool ready = g_face.agent.capabilities_known && !g_face.card_n;
    face_unlock();
    if (ready) app_guide_start();
}

static bool complete(void) {
    esp_err_t err = settings_complete_guide();
    face_lock();
    g_face.agent.guide_save_failed = err != ESP_OK;
    face_unlock();
    if (err != ESP_OK) ESP_LOGW("guide", "could not save completion: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

void app_guide_tap(int hit) {
    face_lock();
    unsigned step = g_face.agent.guide_step;
    if (hit == AGENT_HIT_GUIDE_NEXT || (hit == AGENT_HIT_BACK && step))
        agent_menu_guide_move(&g_face.agent, hit == AGENT_HIT_BACK ? -1 : 1);
    face_unlock();
    if (hit == AGENT_HIT_BACK && !step) {
        face_lock(); agent_menu_hide(&g_face.agent); face_unlock();
        menu_sync();
        return;  // A return is not completion; the unfinished tour remains available next boot.
    }
    bool leave = hit == AGENT_HIT_GUIDE_SKIP || hit == AGENT_HIT_GUIDE_DONE;
    if (!leave || !complete()) return;
    if (hit == AGENT_HIT_GUIDE_DONE) {
        face_lock(); agent_menu_hide(&g_face.agent); face_unlock();
        menu_sync();
    } else menu_close(true);
}
