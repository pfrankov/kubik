#include "app_lab.h"
#include "app_journal.h"
#include "app_internal.h"

#include <string.h>

#include "audio.h"
#include "board.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "ui_text.h"

static const char *TAG = "app";

static void handle_boot_short(void) {
    if (power_is_dark(s_power)) return;
    wake(false);
    if (s_setup) {
        setup_leave();
        audio_sfx(SFX_MENU_CLOSE);
    } else if (s_pair_code[0] && !s_pair_hidden) {
        s_pair_hidden = true;
    } else if (s_menu) {
        menu_back();
    } else {
        face_lock();
        face_card(&g_face, NULL);
        face_unlock();
    }
    disp_wake();
}

static void handle_boot_long(const app_ev_t *e) {
    if (e->a == 1) {
        menu_close(false);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    if (!s_setup) {
        if (s_menu) menu_close(true);
        else menu_open();
    } else if (g_settings.wifi_ssid[0] && s_setup_phase != SETUP_TRYING && s_setup_phase != SETUP_DONE) {
        setup_leave();
        audio_sfx(SFX_SCREEN_OFF);
        bubble(BUB_STOP, STR_SETUP_CLOSED, 2.5f);
    }
}

static void handle_power_short(void) {
    if (power_is_dark(s_power)) {
        audio_sfx(SFX_SCREEN_ON);
    } else {
        menu_close(false);
        audio_sfx(SFX_SCREEN_OFF);
    }
    power_fire(PE_BUTTON, false);
}

static void handle_menu_swipe(void) {
    if (!g_face.journal.open && !g_face.status.open && s_menu_drag_row < 0) menu_close(true);
}

static bool handle_control_event(const app_ev_t *e) {
    switch (e->type) {
    case EV_PWR_LONG:
        menu_close(false);
        power_off_begin();
        return true;
    case EV_PTT_DOWN:
        menu_close(false);
        ptt_down();
        return true;
    case EV_SWIPE_UP:
        handle_menu_swipe();
        return true;
    case EV_MENU_HOLD:
        menu_hold(e->a, e->b);
        return true;
    case EV_DRAG:
        if (s_menu) menu_drag(e->a & 0xFFFF, e->b, e->a >> 16);
        return true;
    case EV_PTT_UP:
        ptt_up();
        return true;
    case EV_BOOT_SHORT:
        handle_boot_short();
        return true;
    case EV_BOOT_LONG:
        handle_boot_long(e);
        return true;
    case EV_SETUP_START:
        setup_enter((setup_reason_t)e->a);
        return true;
    case EV_PWR_SHORT:
        handle_power_short();
        return true;
    default:
        return false;
    }
}

static bool is_touch_event(app_ev_type_t type) {
    return type == EV_TAP || type == EV_PET || type == EV_SHAKE || type == EV_PICKUP || type == EV_TOUCH_DOWN;
}

static void handle_touch_event(const app_ev_t *e) {
    if (e->type == EV_TOUCH_DOWN) {  // contact keeps a visible screen bright; the gesture acts separately
        s_last_touch_or_key = now_ms();
        wake(false);
    } else if (s_setup || (s_pair_code[0] && !s_pair_hidden)) {
        if (e->type != EV_PICKUP) s_last_touch_or_key = now_ms();
        wake(false);
    } else if (s_menu) {
        if (e->type == EV_TAP) {
            s_last_touch_or_key = now_ms();
            wake(false);
            menu_tap(e->a, e->b);
        }
    } else {
        app_touch_event(e);
    }
}

static void handle_link_down(void) {
    app_voice_live_stop(false);
    app_wake_suspend();
    if (s_online) s_down_ms = now_ms();
    s_online = false;
    s_srv = SS_IDLE;
    s_act_own = s_act_other = ACT_NONE;
    face_lock();
    g_face.cron_running = 0;
    g_face.cron_due = -1;
    face_unlock();
    menu_sync();
    if (talk_is_listening(s_talk)) { audio_mic_gate(false, NULL); app_wake_recording_stop(); }
    talk_fire(TE_ABORT);
    stop_speech(false);
}

static void handle_pair_event(void) {
    char code[sizeof s_pair_rx];
    portENTER_CRITICAL(&s_pair_mux);
    memcpy(code, s_pair_rx, sizeof code);
    portEXIT_CRITICAL(&s_pair_mux);
    s_pair_seen_ms = now_ms();
    if (s_setup || !code[0] || !strcmp(code, s_pair_code)) return;
    ESP_LOGI(TAG, "pairing code %s: approve with `openclaw pairing approve kubik %s`", code, code);
    bool first = !s_pair_code[0];
    memcpy(s_pair_code, code, sizeof s_pair_code);
    s_pair_hidden = false;
    wake(false);
    if (first) audio_sfx(SFX_SETUP_PHONE);
}

static bool handle_link_event(const app_ev_t *e) {
    switch (e->type) {
    case EV_LINK_UP:
        ESP_LOGI(TAG, "link up via %s, waiting for welcome", link_via());
        return true;
    case EV_LINK_DOWN:
        if (strcmp(link_via(), "none")) return true; // a queued disconnect cannot cancel the replacement route
        handle_link_down();
        app_journal_event(e);
        return true;
    case EV_PAIR:
        handle_pair_event();
        return true;
    default:
        return false;
    }
}

static void handle_cron_event(const app_ev_t *e) {
    ESP_LOGI(TAG, "cron: %d running, next one-shot in %d s", e->a, e->b);
    face_lock();
    g_face.cron_running = e->a;
    g_face.cron_due = e->b >= 0 ? g_face.t + e->b : -1;
    face_unlock();
    app_journal_event(e);
    // A scheduler snapshot is passive status. Actual notifications wake separately.
}

static void handle_welcome_event(const app_ev_t *e) {
    ESP_LOGI(TAG, "online via %s", link_via());
    s_online = true;
    s_srv_progress = e->b;
    bool paired_now = s_pair_code[0] != 0;
    s_pair_code[0] = 0;
    s_pair_hidden = false;
    if (s_offline_icon) bubble(BUB_NONE, -1, 0);
    s_offline_icon = 0;
    if (e->a >= 0 && e->a <= 100) {
        g_settings.volume = e->a;
        audio_set_volume(e->a);
    }
    if (paired_now) {
        audio_sfx(SFX_SETUP_OK);
        face_emo(EMO_JOY, 4.f);
        bubble(BUB_OK, STR_PAIRED, 3.f);
        bubble_after(BUB_TALK, STR_HOLD_TO_TALK, 8.f);
    } else if (!g_settings.greeted) {
        audio_sfx(SFX_HELLO);
        face_emo(EMO_JOY, 3.f);
        bubble(BUB_OK, STR_CONNECTED, 2.5f);
        bubble_after(BUB_TALK, STR_HOLD_TO_TALK, 8.f);
    } else if (s_outage || (!s_ever_online && now_ms() - s_boot_ms > OUTAGE_MS)) {
        audio_sfx(SFX_CONNECT);
        face_emo(EMO_HAPPY, 1.5f);
        bubble(BUB_OK, STR_CONNECTED, 2.5f);
    } else if (!s_ever_online) {
        face_emo(EMO_HAPPY, 1.5f);
    }
    s_outage = false;
    menu_sync();
    app_agent_refresh_if_open();
    device_state_volume_report(true);
    if (!g_settings.greeted) {
        g_settings.greeted = true;
        settings_save();
    }
    s_ever_online = true;
    app_journal_event(e);
}

static void handle_state_event(const app_ev_t *e) {
    if (e->session && e->session != link_session()) return;
    s_srv = (srv_state_t)e->a;
    if (s_srv != SS_IDLE) wake(false);
    if (s_srv == SS_IDLE) talk_fire(TE_RESOLVE);
    app_journal_event(e);
}

static void handle_activity_event(const app_ev_t *e) {
    if (e->a != s_act_own || e->b != s_act_other)
        ESP_LOGI(TAG, "activity own=%s other=%s", app_activity_names[e->a], app_activity_names[e->b]);
    bool new_work = (e->a && e->a != s_act_own) || (e->b && e->b != s_act_other);
    s_act_own = e->a;
    s_act_other = e->b;
    if (new_work) wake(false);
    if (s_act_own) s_await_t0 = now_ms();
    app_journal_event(e);
}

static void handle_speak_event(const app_ev_t *e) {
    if (!app_speech_matches(e->a, e->session)) return;
    talk_fire(TE_RESOLVE);
    wake_for_reply();
    if (e->b) {
        audio_sfx(SFX_NOTIFY);
        face_ev(FEV_NOTIFY, 0, 0);
    }
    face_ev(FEV_TALK_START, 0, 0);
    app_journal_event(e);
    s_last_activity = now_ms();
}

static void handle_text_event(const app_ev_t *e) {
    static char text[sizeof s_text_rx];
    portENTER_CRITICAL(&s_text_mux);
    memcpy(text, s_text_rx, sizeof text);
    uint32_t receipt = s_text_receipt, session = s_text_session, revision = s_text_revision;
    bool notify = s_text_notify;
    portEXIT_CRITICAL(&s_text_mux);
    if ((uint32_t)e->a != revision || e->session != session) return;
    if (session && session != link_session()) return;
    if (text[strspn(text, " \t\r\n")]) wake_for_reply();
    talk_fire(TE_RESOLVE);
    if (notify && s_gen < 0 && !audio_stream_playing()) {
        audio_sfx(SFX_NOTIFY);
        face_ev(FEV_NOTIFY, 0, 0);
    }
    face_lock();
    face_card(&g_face, text);
    app_journal_text(text, notify);
    face_unlock();
    if (receipt) {
        char ack[64];
        snprintf(ack, sizeof ack, "{\"t\":\"shown\",\"receipt\":%lu}", (unsigned long)receipt);
        link_send_json_in_session(ack, session);
    }
    s_last_activity = now_ms();
}

static void handle_speak_end_event(const app_ev_t *e) {
    if (app_speech_end(e->a, e->session)) app_journal_event(e);
}

static void handle_error_event(const app_ev_t *e) {
    app_journal_event(e);
    app_voice_live_stop(false);
    if (app_wake_recording()) app_wake_abort(true);
    if (e->a == ERR_VOICE && app_voice_mode() == VOICE_REALTIME) {
        audio_mic_gate(false, NULL); app_wake_recording_stop(); talk_fire(TE_ABORT);
    } else talk_fire(TE_RESOLVE);
    ESP_LOGW(TAG, "server error %d", (int)e->a);
    if (e->a == ERR_STT_EMPTY) {
        bool words = now_ms() - s_last_not_heard > 10 * 60000;
        audio_sfx(SFX_NOT_HEARD);
        bubble(BUB_NOT_HEARD, words ? STR_NOT_HEARD : -1, 3.f);
        s_last_not_heard = now_ms();
        face_ev(FEV_NOT_HEARD, 0, 0);
    } else if (e->a == ERR_BUSY) {
        audio_sfx(SFX_THINK);
        bubble(BUB_BUSY, -1, 2.5f);
        face_emo(EMO_THINKING, 2.f);
    } else if (e->a == ERR_UNAUTHORIZED) {
        audio_sfx(SFX_ERROR);
        bubble(BUB_KEY, STR_NOT_ALLOWED, 5.f);
        face_emo(EMO_SAD, 3.f);
    } else {
        audio_sfx(SFX_ERROR);
        bubble(BUB_ERROR, STR_TRY_AGAIN, 3.5f);
        face_ev(FEV_FAIL, 0, 0);
    }
}

static void handle_set_event(const app_ev_t *e) {
    if (e->a >= 0) {
        g_settings.volume = e->a;
        audio_set_volume(e->a);
        device_state_volume_report(false);
        face_ev(FEV_VOLUME, e->a / 100.f, 0);
        volume_bubble(e->a);
    }
    if (e->b >= 10) {
        g_settings.brightness = e->b;
        if (!power_is_asleep(s_power)) disp_brightness_fade(power_brightness(s_power, e->b), 300);
    }
    settings_save();
    app_journal_event(e);
}

static bool remote_current(const app_ev_t *e) { return !e->session || e->session == link_session(); }
static void handle_speak_cancel_event(const app_ev_t *e) {
    if (app_speech_cancel(e->a, e->session)) app_journal_event(e);
}
static void handle_emotion_event(const app_ev_t *e) {
    face_emo((emotion_t)e->a, e->b > 0 ? e->b / 1000.f : 8.f);
    app_journal_event(e);
}
static bool handle_server_event(const app_ev_t *e) {
    if (!remote_current(e)) return true;
    switch (e->type) {
    case EV_SRV_CRON: handle_cron_event(e); return true;
    case EV_SRV_WELCOME: handle_welcome_event(e); return true;
    case EV_SRV_STATE: handle_state_event(e); return true;
    case EV_SRV_ACTIVITY: handle_activity_event(e); return true;
    case EV_SRV_EMOTION: handle_emotion_event(e); return true;
    case EV_SRV_SPEAK: handle_speak_event(e); return true;
    case EV_SRV_TEXT: handle_text_event(e); return true;
    case EV_SRV_SPEAK_END: handle_speak_end_event(e); return true;
    case EV_SRV_SPEAK_CANCEL: handle_speak_cancel_event(e); return true;
    case EV_SRV_ERROR: handle_error_event(e); return true;
    case EV_SRV_SET: handle_set_event(e); return true;
    default: return false;
    }
}

static void handle_agent_capabilities(const app_ev_t *e) {
    if (e->session && e->session != link_session()) return;
    app_voice_capabilities(e->a, e->b);
    if (e->a || !talk_is_listening(s_talk)) return;
    audio_mic_gate(false, NULL);
    app_wake_recording_stop();
    talk_fire(TE_ABORT);
}

static bool dark_input_ignored(const app_ev_t *e) {
    app_ev_type_t type = e->type;
    if (type == EV_BOOT_LONG && e->a == 1) return false; // internal service reboot
    bool key_or_touch = type >= EV_PTT_DOWN && type <= EV_PICKUP &&
                        type != EV_PWR_SHORT && type != EV_PWR_LONG;
    bool gesture = type >= EV_SWIPE_UP && type <= EV_VOICE_WAKE;
    return power_is_dark(s_power) && (key_or_touch || gesture || type == EV_SETUP_START);
}

static bool intercept_input(const app_ev_t *e) {
    return dark_input_ignored(e) || app_lab_event(e);
}

void app_handle(const app_ev_t *e) {
    if (intercept_input(e)) return;
    if (s_power_off_failed && e->type == EV_PWR_SHORT) esp_restart();
    if (s_power_off_at && e->type != EV_PWR_LONG) return;
    if (e->type == EV_VOICE_WAKE) { app_wake_event((uint32_t)e->a); return; }
    if (e->type == EV_VOICE_END) { app_wake_end_event(e->a, e->b); return; }
    if (e->type == EV_INPUT_END) { app_voice_input_end(e->a, e->b, e->session); return; }
    if (handle_control_event(e)) return;
    if (is_touch_event(e->type)) {
        handle_touch_event(e);
        return;
    }
    if (handle_link_event(e)) return;
    if (e->type == EV_AGENT_CAPS) {
        handle_agent_capabilities(e);
        return;
    }
    handle_server_event(e);
}
