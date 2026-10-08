#include "app_internal.h"

#include <stdio.h>

#include "audio.h"
#include "board.h"
#include "esp_log.h"
#include "esp_random.h"
#include "link_poke.h"
#include "freertos/task.h"
#include "ui_text.h"
#include "wifi.h"
#include "tls_mem.h"
#include "muse_pair.h"
#include "tess.h"

static const char *TAG = "app";

static bool fallen_character(void) {
    face_lock();
    bool fallen = face_character(&g_face) == CHARACTER_TESS &&
        (g_face.tess_fallen || g_face.mode == MODE_OFFLINE);
    face_unlock();
    return fallen;
}

void ptt_down(void) {
    app_wake_suspend();
    ESP_LOGI(TAG, "key down");
    s_last_touch_or_key = now_ms();
    wake(false);
    if (muse_pair_key()) { s_ignore_up = true; return; }
    if (app_voice_live_active()) { s_ignore_up = true; app_voice_live_stop(true); return; }
    if (s_setup || s_pair_code[0]) {  // no talking during setup: the screen says what to do
        audio_sfx(SFX_TAP);
        return;
    }
    if (s_menu) menu_close(false);  // PTT always leaves settings before it starts a turn.
    if (s_talk == TALK_LATCHED) {
        s_ignore_up = true;
        finish_turn(TE_PRESS);
        return;
    }
    face_lock();
    face_card(&g_face, NULL);  // talking again: the old card goes
    face_unlock();
    if (!s_online) {
        audio_sfx(SFX_ERROR);
        offline_bubble(4.f);
        face_emo(EMO_SAD, 2.5f);
        return;
    }
    if (!app_agent_mic_allowed()) {
        face_lock();
        face_card(&g_face, "Speech input is not available yet. Open Settings, then Agent, to check STT.");
        face_unlock();
        audio_sfx(SFX_ERROR);
        return;
    }
    if (!tls_mem_turn(!strcmp(link_via(), "wifi"))) {
        face_lock(); face_card(&g_face, "Voice memory is busy. Try KEY again."); face_unlock();
        audio_sfx(SFX_ERROR); return;
    }
    stop_speech(true);
    s_turn++;
    app_voice_prepare_capture(s_turn);
    talk_fire(TE_PRESS);
    s_ptt_t0 = now_ms();
    disp_wake();  // the face shows listening now; the microphone opens after the start chirp
    audio_sfx(SFX_LISTEN_START);
    send_json("{\"t\":\"ptt\",\"on\":true,\"turn\":%d}", s_turn);
    // Open the capture gate as the start chirp leaves the speaker; whatever of it is
    // still audible is muted in the microphone task before its callback.
    wifi_set_fast(true);
    vTaskDelay(pdMS_TO_TICKS(140));
    if (!app_voice_start_capture()) {
        talk_fire(TE_ABORT);
        send_json("{\"t\":\"cancel\",\"turn\":%d}", s_turn);
        face_lock(); face_card(&g_face, "Could not start the microphone. Try KEY again."); face_unlock();
        audio_sfx(SFX_ERROR);
        return;
    }
    ESP_LOGI(TAG, "listening (turn %d)", s_turn);
}

void ptt_up(void) {
    s_last_touch_or_key = now_ms();
    if (s_ignore_up) {
        s_ignore_up = false;
        return;
    }
    if (app_voice_live_active()) { talk_fire(TE_RELEASE_SHORT); return; }
    if (s_talk != TALK_HOLD) return;
    if (now_ms() - s_ptt_t0 >= LATCH_MS) {
        finish_turn(TE_RELEASE_LONG);
        return;
    }
    // A quick press: keep recording until the next press (or a tap).
    talk_fire(TE_RELEASE_SHORT);
    audio_sfx(SFX_LATCH_START);
    ESP_LOGI(TAG, "recording latched (turn %d)", s_turn);
}

void volume_bubble(int level) {
    char text[32];
    snprintf(text, sizeof text, "%s %d%%", str(STR_VOLUME), level);
    bubble_raw(BUB_VOLUME, text, 1.8f);
}

static void touch_tap(const app_ev_t *e) {
    ESP_LOGI(TAG, "tap at %d,%d", (int)e->a, (int)e->b);
    s_last_touch_or_key = now_ms();
    if (!power_is_active(s_power)) {
        wake(true);
        return;
    }
    wake(false);
    face_lock();
    tess_games_set_available(&g_face, app_games_available());
    face_unlock();
    if (app_voice_live_active()) { app_voice_live_stop(true); return; }
    if (s_talk == TALK_LATCHED) {
        finish_turn(TE_TAP);
        return;
    }
    face_lock();
    int page = g_face.card_page;
    bool card = face_card_tap(&g_face);
    bool turned = g_face.card_page != page;
    face_unlock();
    if (card) {
        audio_sfx(turned ? SFX_PAGE : SFX_DISMISS);
        return;
    }
    if (fallen_character()) return;
    if (s_gen >= 0 || audio_stream_playing()) {
        audio_sfx(SFX_DISMISS);
        stop_speech(true);
        face_ev(FEV_TAP, e->a, e->b);
        bubble(BUB_STOP, STR_STOPPED, 1.8f);
        return;
    }
    face_lock();
    bool playing = tess_games_active(&g_face);
    face_event(&g_face, FEV_TAP, e->a, e->b);
    playing = playing || tess_games_active(&g_face);
    face_unlock();
    if (playing) return;
    audio_sfx((esp_random() & 3) == 0 ? SFX_GIGGLE : SFX_TAP);
    if (s_online) link_poke(POKE_TAP);
}

static void touch_pet(const app_ev_t *e) {
    s_last_touch_or_key = now_ms();
    wake(false);
    face_ev(FEV_PET, e->a, e->b);
    audio_sfx(SFX_PET);
    if (s_online) link_poke(POKE_PET);
}

static void touch_shake(void) {
    wake(false);
    face_ev(FEV_SHAKE, 0, 0);
    audio_sfx(SFX_DIZZY);
    if (s_online) link_poke(POKE_SHAKE);
}

static void touch_pickup(const app_ev_t *e) {
    if (now_ms() - s_last_touch_or_key < 1500) return;
    if (e->a >= PICKUP_LONG_S && !talk_is_listening(s_talk) && s_gen < 0) {
        wake(false);
        face_ev(FEV_PICKUP, 1, 0);
        audio_sfx(SFX_SURPRISE);
        s_pickup_sfx_at = now_ms() + 850;
        if (s_online) link_poke(POKE_PICKUP);
        return;
    }
    wake(true);
    face_ev(FEV_PICKUP, 0, 0);
    audio_sfx(SFX_SURPRISE);
}

void app_touch_event(const app_ev_t *e) {
    if (fallen_character()) {
        if (e->type == EV_TAP) touch_tap(e);
        else wake(false);
        return;
    }
    if (e->type == EV_TAP || e->type == EV_PET) audio_tess_gesture(e->a, e->b);
    switch (e->type) {
    case EV_TAP: touch_tap(e); break;
    case EV_PET: touch_pet(e); break;
    case EV_SHAKE: touch_shake(); break;
    case EV_PICKUP: touch_pickup(e); break;
    default: break;
    }
}

// All credentials, device identity and preferences are erased. Restart into first-run setup; the new key needs pairing again.
esp_err_t factory_reset(void) {
    esp_err_t err = settings_factory_reset();
    ESP_LOGW(TAG, "factory reset: %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        // Reboot is queued. The app's next flush must not restore erased wins.
        face_lock();
        tess_games_restore(&g_face, 0);
        tess_games_set_available(&g_face, false);
        face_unlock();
        app_post(EV_BOOT_LONG, 1, 0);  // the restart path of "reboot"
    }
    return err;
}

// PWR held 2 s: Kubik shows "Turning off", the screen fades, then the PMIC cuts the rails.
// Only another PWR hold of about 2 s turns them on. A 4 s hold is the hardware fallback if this hangs.
void power_off_begin(void) {
    app_voice_live_stop(true);
    app_wake_abort(true);
    app_wake_suspend();
    if (s_power_off_at) return;
    ESP_LOGI(TAG, "PWR held: powering off");
    if (talk_is_listening(s_talk)) audio_mic_gate(false, NULL);
    talk_fire(TE_ABORT);
    stop_speech(true);
    wake(false);
    power_fire(PE_GOODBYE, false);  // the character dozes off
    face_emo(EMO_SLEEPY, -1);
    bubble(BUB_POWER, STR_TURNING_OFF, -1);
    s_status_icon = BUB_NONE;
    audio_sfx(SFX_POWER_OFF);
    s_power_off_at = now_ms() + 1700;
}
