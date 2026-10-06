#include "app_wake.h"
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "app_internal.h"
#include "audio.h"
#include "link_mic.h"
#include "auto_voice.h"
#include "esp_log.h"
#include "esp_vad.h"
#include "wake_model.h"
#include "wake_resample.h"
#include "wifi.h"
#include "link.h"
#include "tls_mem.h"
#include "ui_text.h"

static SemaphoreHandle_t s_lock;
static atomic_bool s_listening, s_automatic, s_pending;
static bool s_failed;
static unsigned s_attempts;
static int64_t s_retry_at;
static atomic_bool s_transport_busy, s_retry_reset;
static uint32_t s_epoch;
static int s_auto_turn;
static uint32_t s_auto_session;
static vad_handle_t s_vad;
static auto_voice_t s_voice;
static wake_resample_t s_resample;
static int16_t s_pcm[640];
// Keep up to 80 ms captured AFTER detection while the app handles the wake event.
static int16_t s_postwake[2][MIC_FRAME_SAMPLES];
static unsigned s_postwake_count, s_postwake_next;

void app_wake_init(void) {
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_lock ? ESP_OK : ESP_ERR_NO_MEM);
}
static bool eligible(void) {
    return !app_voice_live_active() && !atomic_load(&s_transport_busy) && power_is_active(s_power) && s_online && app_agent_mic_allowed() &&
           !s_setup && !s_menu && !s_pair_code[0] && !s_power_off_at &&
           s_talk == TALK_IDLE && s_srv == SS_IDLE && s_gen < 0 && !s_act_own;
}
static void suspend_model(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_listening) {
        s_listening = false;
        audio_wake_gate(false, NULL); // waits for the sole capture callback to finish
        wake_model_stop();
        s_pending = false;
    }
    xSemaphoreGive(s_lock);
}
void app_wake_transport_busy(bool busy) {
    atomic_store(&s_transport_busy, busy);
    if (busy) {
        atomic_store(&s_retry_reset, true); // even a reconnect completed between app ticks
        suspend_model();
    }
}
void app_wake_suspend(void) {
    suspend_model();
    if (s_automatic) {
        s_pending = true;
        audio_mic_request_stop();
        app_post(EV_VOICE_END, s_auto_turn, 2); // app owns the turn and VAD destructor
    }
}
static void wake_frame(const int16_t *pcm, int samples, const uint8_t *ima) {
    (void)ima;
    if (!s_listening || samples != MIC_FRAME_SAMPLES) return;
    if (s_pending) {
        memcpy(s_postwake[s_postwake_next], pcm, sizeof s_postwake[0]);
        s_postwake_next = (s_postwake_next + 1) % 2;
        if (s_postwake_count < 2) ++s_postwake_count;
        return;
    }
    size_t n = wake_resample(&s_resample, pcm, samples, s_pcm);
    if (wake_model_feed(s_pcm, n) && !audio_self_audible()) {
        s_pending = true;
        app_post(EV_VOICE_WAKE, (int)s_epoch, 0);
    }
}
static void start_listening(void) {
    ++s_attempts;
    s_failed = s_attempts >= 3; s_retry_at = now_ms() + 5000;
    if (!wake_model_start()) return;
    memset(&s_resample, 0, sizeof s_resample);
    s_postwake_count = s_postwake_next = 0; s_pending = false;
    s_listening = true;
    s_epoch = audio_capture_epoch() + 1;
    if (!audio_wake_gate(true, wake_frame)) {
        s_listening = false;
        audio_wake_gate(false, NULL); wake_model_stop();
    }
}
static bool recording_tick(void) {
    if (!s_automatic) return false;
    if (s_gen >= 0 || !talk_is_listening(s_talk) || link_session() != s_auto_session) app_wake_abort(true);
    return s_automatic;
}
static bool ready_to_start(void) {
    return !s_listening && !s_failed && now_ms() >= s_retry_at && !audio_self_audible() && !audio_sfx_playing();
}
static void reset_retries(void) { s_failed = false; s_attempts = 0; s_retry_at = 0; }
void app_wake_tick(void) {
    if (atomic_exchange(&s_retry_reset, false)) reset_retries();
    if (recording_tick()) return;
    if (!eligible()) {
        app_wake_suspend(); reset_retries(); return;
    }
    if (s_listening && !wake_model_healthy()) { suspend_model(); s_retry_at = now_ms() + 5000; }
    if (!ready_to_start()) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (eligible() && !s_listening) {
        start_listening();
    }
    xSemaphoreGive(s_lock);
}
static void automatic_frame(const int16_t *pcm, int samples, const uint8_t *ima) {
    if (!pcm) { app_voice_capture_failed(); return; }
    if (!s_automatic || s_pending || samples != MIC_FRAME_SAMPLES) return;
    if (!link_send_mic_in_session(s_auto_turn, pcm, samples * 2, s_auto_session, ima)) {
        s_pending = true; app_post(EV_VOICE_END, s_auto_turn, 2); return;
    }
    size_t n = wake_resample(&s_resample, pcm, samples, s_pcm);
    for (size_t off = 0; off + 320 <= n; off += 320) {
        bool muted = audio_self_audible();
        bool speech = !muted && vad_process(s_vad, s_pcm + off, 16000, 20) == VAD_SPEECH;
        auto_voice_result_t result = auto_voice_step(&s_voice, speech, muted, 20);
        if (result == AUTO_CONTINUE) continue;
        s_pending = true;
        app_post(EV_VOICE_END, s_auto_turn, result == AUTO_EMPTY);
        break;
    }
}
void app_wake_recording_stop(void) {
    // All callers have already closed the capture gate and drained its callback.
    s_automatic = false; s_pending = false;
    if (s_vad) { vad_destroy(s_vad); s_vad = NULL; }
    app_voice_recording_stop();
}
static bool start_upload(uint32_t capture_epoch) {
    char message[80];
    snprintf(message, sizeof message, "{\"t\":\"ptt\",\"on\":true,\"turn\":%d,\"automatic\":true}", s_turn);
    if (s_gen >= 0 || !link_send_json_in_session(message, s_auto_session)) return false;
    wifi_set_fast(true); disp_wake();
    // No start chirp: it would mute the start of a command spoken without a pause.
    unsigned first = s_postwake_count == 2 ? s_postwake_next : 0;
    for (unsigned i = 0; i < s_postwake_count; i++) {
        if (!link_send_mic_in_session(s_auto_turn, s_postwake[(first + i) % 2], sizeof s_postwake[0], s_auto_session, NULL)) return false;
    }
    bool started = !s_pending && s_gen < 0 && audio_mic_start_epoch(automatic_frame, capture_epoch);
    if (started) app_voice_capture_started(s_turn);
    return started;
}
static void start_live_wake(void) {
    s_last_touch_or_key = now_ms(); wake(false);
    face_lock(); face_card(&g_face, NULL); face_unlock();
    ++s_turn; talk_fire(TE_PRESS); talk_fire(TE_RELEASE_SHORT);
    s_ptt_t0 = now_ms(); app_voice_prepare_capture(s_turn);
    send_json("{\"t\":\"ptt\",\"on\":true,\"turn\":%d,\"automatic\":true}", s_turn);
    wifi_set_fast(true); disp_wake();
    // Eligible wake capture has no speaker output. Preserve the 80 ms command
    // preroll with a silent reference, then capture both codec channels continuously.
    unsigned first = s_postwake_count == 2 ? s_postwake_next : 0;
    for (unsigned i = 0; i < s_postwake_count; ++i) {
        const uint8_t *ima = link_mic_duplex_preroll(s_postwake[(first + i) % 2]);
        if (!link_send_duplex_in_session(s_turn, ima, link_session())) { app_voice_live_stop(true); return; }
    }
    if (!app_voice_start_capture()) app_voice_live_stop(true);
}
void app_wake_event(uint32_t epoch) {
    if (!s_listening || !s_pending || epoch != s_epoch || epoch != audio_capture_epoch() || !eligible()) return;
    app_wake_suspend();
    uint32_t capture_epoch = audio_capture_epoch();
    if (!tls_mem_turn(!strcmp(link_via(), "wifi"))) return;
    if (app_voice_mode() == VOICE_LIVE) { start_live_wake(); return; }
    s_vad = vad_create_with_param(VAD_MODE_3, 16000, 20, 20, 20);
    if (!s_vad) { ESP_LOGE("wake", "VAD allocation failed"); return; }
    reset_retries(); // a recognized turn ends this continuous inference interval
    s_last_touch_or_key = now_ms(); wake(false);
    face_lock(); face_card(&g_face, NULL); face_unlock();
    ++s_turn; s_auto_turn = s_turn;
    talk_fire(TE_PRESS); talk_fire(TE_RELEASE_SHORT);
    s_ptt_t0 = now_ms(); s_voice = (auto_voice_t){.warmup_ms = 160};
    memset(&s_resample, 0, sizeof s_resample);
    s_pending = false; s_automatic = true;
    s_auto_session = link_session();
    app_voice_prepare_capture(s_turn);
    if (!start_upload(capture_epoch)) { app_wake_abort(true); return; }
    ESP_LOGI("wake", "automatic turn %d", s_turn);
}
void app_wake_abort(bool tell_server) {
    if (!s_automatic) return;
    audio_mic_gate(false, NULL); app_wake_recording_stop(); talk_fire(TE_ABORT);
    if (tell_server) {
        char message[48]; snprintf(message, sizeof message, "{\"t\":\"cancel\",\"turn\":%d}", s_auto_turn);
        link_send_json_in_session(message, s_auto_session);
    }
}
void app_wake_end_event(int turn, int reason) {
    if (!s_automatic || turn != s_auto_turn || s_talk != TALK_LATCHED) return;
    if (reason == 2 || s_gen >= 0 || link_session() != s_auto_session) { app_wake_abort(true); return; }
    if (!reason) { finish_turn(TE_TAP); return; }
    app_wake_abort(true);
    bubble(BUB_STOP, STR_NOT_HEARD, 2.f); audio_sfx(SFX_NOT_HEARD);
}
uint32_t app_wake_session(void) { return s_automatic ? s_auto_session : 0; }
bool app_wake_recording(void) { return s_automatic; }
bool app_wake_listening(void) { return s_listening; }
uint32_t app_wake_max_us(void) { return wake_model_max_us(); }
uint8_t app_wake_probability(void) { return wake_model_probability(); }

uint8_t app_wake_peak_probability(void) { return wake_model_peak_probability(); }
uint32_t app_wake_average_us(void) { return wake_model_average_us(); }
