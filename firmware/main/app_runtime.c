#include "app_lab.h"
#include "app_status.h"
#include "app_internal.h"
#include "muse_backend.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "board.h"
#include "esp_log.h"
#include "heap_probe.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "input.h"
#include "input_rub.h"
#include "power_network.h"
#include "setup.h"
#include "ui_text.h"
#include "wifi.h"
#include "tls_mem.h"

static const char *TAG = "app";
static power_network_t s_power_network;
static bool s_power_network_ready;
static uint32_t s_network_transitions;
int app_network_phase(void) { return s_power_network_ready ? s_power_network.phase : POWER_NETWORK_ACTIVE; }
uint32_t app_network_transitions(void) { return s_network_transitions; }

// The status bubble for an OpenClaw activity (as its status reactions: 🧠 🛠️ 💻 🌐 🛫 🏗️ 🗜️ ⏳).
static void activity_bubble(int act, bool elsewhere, int *icon, int *text) {
    switch (act) {
    case ACT_TOOL: *icon = BUB_TOOL; *text = STR_USING_TOOLS; break;
    case ACT_CODING: *icon = BUB_CODE; *text = STR_CODING; break;
    case ACT_WEB:
    case ACT_CONCIERGE: *icon = BUB_WEB; *text = STR_BROWSING; break;
    case ACT_DEPLOY: *icon = BUB_DEPLOY; *text = STR_DEPLOYING; break;
    case ACT_BUILD: *icon = BUB_BUILD; *text = STR_BUILDING; break;
    case ACT_COMPACTING: *icon = BUB_COMPACT; *text = STR_COMPACTING; break;
    case ACT_STALL: *icon = BUB_BUSY; *text = STR_STILL_WORKING; break;
    default: *icon = BUB_THINK; *text = elsewhere ? STR_WORKING : STR_THINKING; break;
    }
}

static bool is_status_icon(int icon) { return icon && icon == s_status_icon; }

static void current_status(int *icon, int *text) {
    if (s_setup || s_pair_code[0] || !s_online || app_voice_live_active()) return;
    if (talk_is_listening(s_talk)) {
        *icon = BUB_REC;
        bool automatic = app_wake_recording() || app_voice_mode() != VOICE_CLASSIC;
        *text = automatic ? STR_PAUSE_TO_SEND : s_talk == TALK_LATCHED ? STR_TAP_TO_SEND : STR_LISTENING;
    } else if (s_talk == TALK_AWAITING || s_srv == SS_TRANSCRIBING || s_srv == SS_THINKING || s_act_own) {
        activity_bubble(s_act_own, false, icon, text);
    }
}

static bool other_activity_status(int shown, int *icon, int *text) {
    if (*icon || !s_act_other || s_setup || s_pair_code[0] || !s_online || s_offline_icon ||
        (shown != BUB_NONE && !is_status_icon(shown)) || s_next_bubble.icon)
        return false;
    activity_bubble(s_act_other, true, icon, text);
    return true;
}

static void show_status(int icon, int text, int shown, bool elsewhere) {
    if (icon) {
        if (!elsewhere) s_next_bubble.icon = BUB_NONE;
        bubble((bubble_icon_t)icon, text, -1);
        s_status_icon = icon;
    } else if (is_status_icon(shown)) {
        bubble(BUB_NONE, -1, 0);
        s_status_icon = BUB_NONE;
    }
}

static void update_power_saving(void) {
    bool checking = app_network_phase() == POWER_NETWORK_CHECK;
    bool joining = wifi_radio_started() && !s_online;
    wifi_set_fast(s_talk != TALK_IDLE || s_gen >= 0 || s_srv != SS_IDLE || checking || joining);
    bool doze = power_is_dark(s_power) && s_talk == TALK_IDLE && s_gen < 0 && s_srv == SS_IDLE;
    audio_doze(doze);
    wifi_set_doze(doze);
    bool diagnostic = link_usb_host_present() && now_ms() < s_debug_network_until;
    int quiet_reads = diagnostic ? LIGHT_SLEEP_QUIET_READS : s_quiet_reads;
    power_light_sleep_allow(light_sleep_allowed(doze, quiet_reads, link_usb_host_present() && !diagnostic,
                                               wifi_radio_started() || checking));
}

static void status_tick(void) {
    if (s_talk == TALK_IDLE && s_srv == SS_IDLE) tls_mem_turn(false);
    update_power_saving();
    app_status_refresh();
    int icon = BUB_NONE, text = -1;
    current_status(&icon, &text);
    face_lock();
    int shown = g_face.bub_icon;
    face_unlock();
    bool elsewhere = other_activity_status(shown, &icon, &text);
    int key = icon * 64 + text + 1;
    if (key == s_status_key && (icon == BUB_NONE || shown == icon)) {
        if (!shown && s_next_bubble.icon) {
            bubble((bubble_icon_t)s_next_bubble.icon, s_next_bubble.text, s_next_bubble.seconds);
            s_next_bubble.icon = BUB_NONE;
        }
        return;
    }
    s_status_key = key;
    show_status(icon, text, shown, elsewhere);
}

static bool recovery_close_due(int64_t t, setup_phase_t previous, int64_t *started) {
    if (!s_setup || s_setup_reason != SETUP_LOST) return false;
    if (previous == SETUP_TRYING && s_setup_phase == SETUP_FAILED) *started = t;
    int grace = 120000 + 480000 * (wifi_ap_clients() > 0);
    return t - *started > grace && setup_claim_idle_close();
}

static bool tick_recovery(void) {
    static bool offered_for_outage;
    static int64_t recovery_started_ms;
    static setup_phase_t previous_phase = SETUP_OFF;
    if (power_network_scheduled(&s_power_network)) {
        previous_phase = s_setup_phase;
        return false;
    }
    if (wifi_sta_connected()) {
        offered_for_outage = false;
        previous_phase = s_setup_phase;
        return false;
    }
    if (recovery_close_due(now_ms(), previous_phase, &recovery_started_ms)) {
        setup_leave();
        power_fire(PE_DARK_NOW, false);
        return false;
    }
    previous_phase = s_setup_phase;
    if (power_is_dark(s_power) || s_setup || s_online || offered_for_outage || !wifi_take_recovery_request()) return false;
    offered_for_outage = true;
    recovery_started_ms = now_ms();
    setup_enter(SETUP_LOST);
    return true;
}

static bool tick_power_off(int64_t t) {
    if (!s_power_off_at) return false;
    if (t >= s_power_off_at) {
        ESP_LOGI(TAG, "power off");
        disp_power(false);
        vTaskDelay(pdMS_TO_TICKS(50));
        pmic_power_off();
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGW(TAG, "power is still on after power off");
        s_power_off_failed = true;
        s_power_off_at = t + 3600000;
    }
    return true;
}

static void tick_ui_timeouts(int64_t t) {
    if (s_setup) setup_step();
    app_agent_tick(t);
    face_lock();
    bool guide = g_face.journal.open || (g_face.agent.open && g_face.agent.view == AGENT_VIEW_GUIDE);
    face_unlock();
    if (s_menu && t - s_menu_touch_ms > (guide || app_lab_active() ? 120000 : 30000)) menu_close(true);
    if (s_armed_ms && t - s_armed_ms > 3000) {
        s_armed_ms = 0;
        audio_sfx(SFX_DISARM);
        menu_sync();
    }
}

static void tick_pickup_sound(int64_t t) {
    if (!s_pickup_sfx_at || t < s_pickup_sfx_at) return;
    s_pickup_sfx_at = 0;
    if (!talk_is_listening(s_talk) && s_gen < 0) audio_sfx(SFX_GIGGLE);
}

static void tick_pair_expiry(int64_t t) {
    if (s_pair_code[0] && link_last_problem() != LINK_PAIRING && t - s_pair_seen_ms > 20000)
        s_pair_code[0] = 0;
}

static void tick_outage(int64_t t) {
    if (!s_online && s_ever_online && !s_outage && t - s_down_ms > OUTAGE_MS) {
        s_outage = true;
        ESP_LOGW(TAG, "offline for %d s", OUTAGE_MS / 1000);
        if (!power_is_asleep(s_power) && !s_setup) audio_sfx(SFX_DISCONNECT);
    }
}

static void tick_offline_bubble(int64_t t) {
    static bool previous_clock_ready;
    bool told = s_ever_online ? s_outage : t - s_boot_ms > 20000;
    if (!s_online && told && !s_setup && !s_pair_code[0] && !power_is_asleep(s_power) && g_settings.wifi_ssid[0]) {
        int icon = offline_icon();
        bool clock_ready = wifi_time_ready();
        int status = icon * 2 + clock_ready;
        int previous_status = s_offline_icon * 2 + previous_clock_ready;
        if (status != previous_status) {
            s_offline_icon = icon;
            previous_clock_ready = clock_ready;
            offline_bubble(-1);
        }
    } else if (s_offline_icon && (s_online || s_setup || s_pair_code[0])) {
        s_offline_icon = 0;
        bubble(BUB_NONE, -1, 0);
    }
}

static void tick_connection(int64_t t) {
    tick_pair_expiry(t);
    if (power_network_scheduled(&s_power_network)) return;
    tick_outage(t);
    tick_offline_bubble(t);
}

static void tick_playback(int64_t t) {
    if (app_speech_tick(t, s_srv_progress)) s_last_activity = t;
}

static void tick_conversation(int64_t t) {
    if (s_talk == TALK_AWAITING && s_think_cues < 3 && t - s_await_t0 > 2500 + s_think_cues * 4000) {
        s_think_cues++;
        audio_sfx(SFX_THINK);
    }
    if (!app_voice_live_active() && s_talk == TALK_LATCHED && t - s_ptt_t0 > LATCH_MAX_MS) finish_turn(TE_LATCH_TIMEOUT);
    if (!app_voice_live_active() && s_talk == TALK_AWAITING && t - s_await_t0 > 45000) {
        talk_fire(TE_RESOLVE);
        audio_sfx(SFX_ERROR);
        bubble(BUB_ERROR, STR_NO_ANSWER, 4.f);
        face_ev(FEV_FAIL, 0, 0);
    }
}

static bool activity_pending(void) {
    face_lock();
    bool card = g_face.card_n > 0;
    face_unlock();
    return app_voice_live_active() || s_talk != TALK_IDLE || s_act_own || s_gen >= 0 || audio_stream_playing() ||
           s_setup || s_pair_code[0] || card || s_menu;
}

static power_network_inputs_t network_sleep_inputs(void) {
    bool diagnostic = now_ms() < s_debug_network_until && link_usb_host_present();
    return (power_network_inputs_t){
        .screen_dark = power_is_dark(s_power),
        .usb_absent_known = diagnostic || s_pmic_vbus_known_false,
        .charging = !diagnostic && s_pmic_charging,
        .setup_active = s_setup,
        .access_point_active = wifi_ap_active() || wifi_ap_clients() > 0,
        .usb_host_active = !diagnostic && link_usb_host_present(),
        .work_active = app_voice_live_active() || s_talk != TALK_IDLE || s_gen >= 0 || audio_stream_playing() ||
                       s_srv != SS_IDLE || s_act_own || muse_backend_selected(),
        .saved_networks = g_settings.wifi_profile_count > 0,
    };
}

static void tick_network_power(int64_t t) {
    if (!s_power_network_ready) {
        power_network_init(&s_power_network, t);
        s_power_network_ready = true;
    }
    power_network_inputs_t inputs = network_sleep_inputs();
    power_network_result_t result = power_network_step(&s_power_network, t, &inputs);
    if (result.checking && wifi_poll_join_expired()) {
        link_set_wifi_allowed(false);
        wifi_radio_stop();
        ESP_LOGI(TAG, "Wi-Fi check: join deadline expired; queued events wait for next cycle");
    }
    if (result.action != POWER_NETWORK_NO_ACTION) s_network_transitions++;
    if (result.action == POWER_NETWORK_RADIO_OFF) {
        link_set_wifi_allowed(false);
        wifi_radio_stop();
        ESP_LOGI(TAG, "Wi-Fi radio paused; next server check in %d s",
                 (int)((s_power_network.next_check_ms - t) / 1000));
    } else if (result.action == POWER_NETWORK_RADIO_ON) {
        wifi_radio_resume(result.checking);
        if (!s_setup) link_set_wifi_allowed(true);
        if (result.checking) ESP_LOGI(TAG, "Wi-Fi server check started");
        else ESP_LOGI(TAG, "Wi-Fi radio resumed");
    }
}

static void tick_sleep(int64_t t) {
    bool busy = activity_pending();
    bool debug = t < s_debug_until;
    if ((debug && s_power != PWR_AWAKE) || (busy && s_power == PWR_DIMMED)) wake(false);
    if (busy || debug) s_last_activity = t;
    power_event_t idle = power_idle_event(s_power, t - s_last_activity);
    if (idle != PE_NONE) power_fire(idle, false);
}

// The app task's tick: relaxed while the screen is dark and nothing is going on (events still wake it).
int app_tick_ms(void) {
    bool quiet = power_is_dark(s_power) && s_talk == TALK_IDLE && s_gen < 0 && s_srv == SS_IDLE;
    return quiet ? 250 : 50;
}

void app_tick(void) {
    int64_t t = now_ms();
    if (tick_power_off(t)) return;
    tick_ui_timeouts(t);
    app_guide_tick();
    tick_network_power(t);
    if (tick_recovery()) return;
    status_tick();
    tick_pickup_sound(t);
    tick_connection(t);
    tick_playback(t);
    tick_conversation(t);
    tick_sleep(t);
}

static void setup_face(face_t *f) {
    static setup_labels_t labels;  // UI task only
    char detail[72];
    int n = 0, step = 0;
    const uint8_t *qr = setup_qr(&n, &step);
    setup_labels(&labels);
    if (s_setup_phase == SETUP_TRYING) {
        snprintf(detail, sizeof detail, "\u201C%.32s\u201D", labels.trying);
        face_set_setup(f, NULL, 0, 3, "Connecting\u2026", detail);
    } else if (step == 2) {
        face_set_setup(f, qr, n, 2, "Scan again for setup", "192.168.4.1");
    } else {
        const char *title = s_setup_reason == SETUP_LOST ? "Scan to reconnect" : "Scan to join Wi-Fi";
        snprintf(detail, sizeof detail, "%s  \u00B7  password %s", labels.ap_ssid, labels.ap_pass);
        face_set_setup(f, qr, n, 1, title, detail);
    }
    memset(&labels, 0, sizeof labels);
    memset(detail, 0, sizeof detail);
}

static bool speech_pose_active(void) {
    return audio_stream_playing() || (s_gen >= 0 && audio_stream_played_ms() > 0);
}

static bool waiting_for_reply(void) {
    return s_talk == TALK_AWAITING || s_srv == SS_TRANSCRIBING || s_srv == SS_THINKING || s_gen >= 0 || s_act_own;
}
static bool show_offline(int64_t t) {
    return !s_online && (s_ever_online ? s_outage : t - s_boot_ms > 20000);
}

static face_mode_t awake_face_mode(int64_t t) {
    if (power_is_asleep(s_power)) return MODE_SLEEP;
    if (app_voice_live_active()) {
        if (speech_pose_active()) return MODE_SPEAKING;
        return audio_mic_is_open() ? MODE_LISTENING : MODE_THINKING;
    }
    if (talk_is_listening(s_talk) || audio_mic_is_open()) return MODE_LISTENING;
    // Pauses in one reply keep its pose; the real PCM envelope still falls to zero.
    if (speech_pose_active()) return MODE_SPEAKING;
    if (waiting_for_reply()) return MODE_THINKING;
    if (show_offline(t)) return MODE_OFFLINE;
    return MODE_IDLE;
}

static face_mode_t select_face_mode(face_t *f, int64_t t, bool *debug) {
    face_mode_t mode;
    if (s_pair_code[0] && !s_pair_hidden && !s_setup) {
        face_set_pairing(f, s_pair_code);
        mode = MODE_SETUP;
    } else if (s_setup) {
        face_set_pairing(f, NULL);
        setup_face(f);
        mode = s_setup_phase == SETUP_DONE || s_setup_phase == SETUP_FAILED ? MODE_IDLE : MODE_SETUP;
    } else {
        face_set_pairing(f, NULL);
        mode = awake_face_mode(t);
    }
    *debug = t < s_debug_until;
    return *debug ? (face_mode_t)s_debug_mode : mode;
}

// The scripted turn of the "spin" test hook: a tilted axis, the angle swinging +-3.6 rad (past a half turn).
static void spin_view(float q[4], float t) {
    float angle = 3.6f * sinf(t * .9f), ax = .6f, ay = .8f * cosf(t * .7f), az = .3f * sinf(t * .4f);
    float s = sinf(angle / 2) / sqrtf(ax * ax + ay * ay + az * az);
    q[0] = cosf(angle / 2); q[1] = ax * s; q[2] = ay * s; q[3] = az * s;
}

static void update_face_motion(face_t *f, int64_t t) {
    input_view_active(face_character(f) == CHARACTER_TESS);
    if (face_character(f) == CHARACTER_TESS) {
        input_view_tilt(&f->tilt_x, &f->tilt_y);
        input_view_quat(f->view_q);
        if (t < s_debug_spin_until) spin_view(f->view_q, t / 1000.f);
    } else {
        input_tilt(&f->tilt_x, &f->tilt_y);
    }
    float dvx, dvy;
    input_jolt(&dvx, &dvy);
    f->jolt_dvx += dvx;
    f->jolt_dvy += dvy;
    input_slide(&f->slide_x, &f->slide_y);
    input_gravity(&f->grav_x, &f->grav_y);
    static bool scripted_rub;
    bool scripting = t < s_debug_rub_until;
    if (scripting || scripted_rub) input_rub_script(fmodf(t / 1000.f, 10.f), face_character(f) == CHARACTER_TESS, scripting);
    scripted_rub = scripting;
    rub_input_t rub;
    input_rub(&rub);  // the face takes the finger's stroke (petting with effort)
    f->rub_in.path += rub.path;
    f->rub_in.turns += rub.turns;
    f->rub_in.x = rub.x;
    f->rub_in.y = rub.y;
    f->rub_in.down = rub.down;
    static int64_t motion_log;
    if ((fabsf(f->loose_x) > 3 || fabsf(f->loose_y) > 3 || fabsf(f->body_dx) > 2) && t - motion_log > 300) {
        motion_log = t;
        ESP_LOGI(TAG, "face slide %.2f,%.2f -> loose %.0f,%.0f sway %.0f", f->slide_x, f->slide_y, f->loose_x,
                 f->loose_y, f->body_dx);
    }
}

static void update_sleep_emotion(face_t *f, face_mode_t mode, int64_t t) {
    if (mode == MODE_IDLE && t - s_last_activity > 120000 && f->emotion == EMO_NEUTRAL)
        face_set_emotion(f, EMO_SLEEPY, -1);
    if (f->emotion == EMO_SLEEPY && f->emotion_left < 0 && (mode != MODE_IDLE || t - s_last_activity < 120000))
        face_set_emotion(f, EMO_NEUTRAL, -1);
}

void app_face_inputs(face_t *f) {
    int64_t t = now_ms();
    if (f->mode == MODE_BOOT) return;
    f->journal.overlay = g_settings.event_overlay;
    f->live_active = app_voice_live_active();
    f->live_ready = app_voice_live_ready();
    f->live_mic = f->live_active && audio_mic_is_open();
    bool debug;
    face_mode_t mode = select_face_mode(f, t, &debug);
    face_set_mode(f, mode);
    f->card_hold = s_gen >= 0 || audio_stream_playing();
    f->mic_level = audio_mic_level();
    f->spk_level = audio_spk_level();
    if (debug) f->mic_level = f->spk_level = 0.5f + 0.4f * sinf(t * 0.03f);
    f->offline_icon = s_offline_icon == BUB_NO_WIFI || s_offline_icon == BUB_NO_SERVER ? s_offline_icon : 0;
    update_face_motion(f, t);
    update_sleep_emotion(f, mode, t);
}
