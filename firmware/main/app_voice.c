#include "app_voice.h"
#include "app_internal.h"
#include "app_speech.h"
#include "agent_protocol.h"
#include "audio.h"
#include "board.h"
#include "auto_voice.h"
#include "esp_vad.h"
#include "wake_resample.h"
#include "ui_text.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

static atomic_int s_mode;
static atomic_bool s_live, s_live_ready;
static portMUX_TYPE s_input_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_input_turn;
static uint32_t s_input_session, s_input_epoch;
static bool s_preparing, s_queued_end;
static vad_handle_t s_vad;
static volatile bool s_ending;
static auto_voice_t s_pause;
static wake_resample_t s_resample;
static int16_t s_pcm[640];
static uint32_t s_input_frames, s_send_ms, s_send_max_ms, s_send_slow;
static int64_t s_input_started_ms;
static uint32_t s_input_dropped_ms;

voice_mode_t app_voice_mode(void) { return (voice_mode_t)atomic_load(&s_mode); }

bool app_voice_live_active(void) { return atomic_load(&s_live); }
bool app_voice_live_ready(void) { return atomic_load(&s_live_ready); }

void app_voice_live_stop(bool tell_server) {
    if (!atomic_exchange(&s_live, false)) return;
    atomic_store(&s_live_ready, false);
    audio_mic_gate(false, NULL);
    app_wake_recording_stop();
    talk_fire(TE_ABORT);
    stop_speech(false);
    if (tell_server) {
        char message[48]; snprintf(message, sizeof message, "{\"t\":\"cancel\",\"turn\":%d}", s_input_turn);
        link_send_json_in_session(message, s_input_session);
    }
    disp_wake();
}

static void request_end(uint8_t turn, uint32_t session);

void app_voice_prepare_capture(uint8_t turn) {
    uint32_t session = link_session(), epoch = audio_capture_epoch();
    portENTER_CRITICAL(&s_input_mux);
    s_input_turn = turn; s_input_session = session; s_input_epoch = epoch;
    atomic_store(&s_live, app_voice_mode() == VOICE_LIVE);
    atomic_store(&s_live_ready, false);
    s_preparing = app_voice_mode() != VOICE_CLASSIC; s_queued_end = false;
    portEXIT_CRITICAL(&s_input_mux);
}

void app_voice_capture_started(uint8_t turn) {
    uint32_t session = link_session(), epoch = audio_capture_epoch();
    portENTER_CRITICAL(&s_input_mux);
    s_input_turn = turn; s_input_session = session; s_input_epoch = epoch;
    atomic_store(&s_live, app_voice_mode() == VOICE_LIVE);
    atomic_store(&s_live_ready, false);
    s_preparing = false;
    bool ended = s_queued_end;
    portEXIT_CRITICAL(&s_input_mux);
    if (ended) request_end(turn, session);
}

static void request_end(uint8_t turn, uint32_t session) {
    portENTER_CRITICAL(&s_input_mux);
    bool matches = session && session == s_input_session && turn == s_input_turn;
    uint32_t epoch = s_input_epoch;
    bool preparing = matches && s_preparing;
    if (preparing) s_queued_end = true;
    portEXIT_CRITICAL(&s_input_mux);
    if (matches && app_voice_live_active()) {
        if (!preparing) audio_mic_request_stop_epoch(epoch);
        app_post_in_session(EV_INPUT_END, turn, 0, session);
        return;
    }
    if (preparing) return; // App owns gate startup; it applies this end before opening or after draining startup.
    if (matches && app_voice_mode() != VOICE_CLASSIC && audio_mic_request_stop_epoch(epoch))
        app_post_in_session(EV_INPUT_END, turn, (int32_t)(epoch + 1), session);
}

static bool send_input(const int16_t *pcm, int samples, uint8_t turn, uint32_t session, const uint8_t *ima) {
    int64_t sent_at = now_ms();
    if (!s_input_frames) s_input_started_ms = sent_at;
    bool sent = app_voice_mode() == VOICE_LIVE
        ? link_send_duplex_in_session(turn, ima, session)
        : link_send_mic_in_session(turn, pcm, samples * 2, session, ima);
    uint32_t send_ms = (uint32_t)(now_ms() - sent_at);
    ++s_input_frames; s_send_ms += send_ms;
    if (send_ms > s_send_max_ms) s_send_max_ms = send_ms;
    if (send_ms > 40) ++s_send_slow;
    return sent;
}

void app_voice_capture_failed(void) {
    portENTER_CRITICAL(&s_input_mux);
    uint8_t turn = s_input_turn;
    uint32_t session = s_input_session;
    portEXIT_CRITICAL(&s_input_mux);
    uint32_t epoch = audio_capture_epoch();
    ESP_LOGE("voice", "microphone delivery overflow; aborting turn");
    if (audio_mic_request_stop_epoch(epoch))
        app_post_in_session(EV_INPUT_END, turn | (2 << 8), (int32_t)(epoch + 1), session);
}

static void classic_frame(const int16_t *pcm, int samples, const uint8_t *ima) {
    if (!pcm) { app_voice_capture_failed(); return; }
    portENTER_CRITICAL(&s_input_mux);
    uint8_t turn = s_input_turn;
    uint32_t session = s_input_session;
    portEXIT_CRITICAL(&s_input_mux);
    uint32_t epoch = audio_capture_epoch();
    if (!link_send_mic_in_session(turn, pcm, samples * 2, session, ima) && audio_mic_request_stop_epoch(epoch))
        app_post_in_session(EV_INPUT_END, turn | (2 << 8), (int32_t)(epoch + 1), session);
}

static void check_pause(const int16_t *pcm, int samples, uint8_t turn, uint32_t session, uint32_t epoch) {
    size_t count = wake_resample(&s_resample, pcm, samples, s_pcm);
    for (size_t at = 0; at + 320 <= count; at += 320) {
        bool muted = audio_self_audible();
        bool speech = !muted && vad_process(s_vad, s_pcm + at, 16000, 20) == VAD_SPEECH;
        auto_voice_result_t result = auto_voice_step(&s_pause, speech, muted, 20);
        if (result == AUTO_CONTINUE) continue;
        s_ending = true;
        if (audio_mic_request_stop_epoch(epoch))
            app_post_in_session(EV_INPUT_END, turn | ((result == AUTO_EMPTY ? 2 : 1) << 8), (int32_t)(epoch + 1), session);
        return;
    }
}

static void native_frame(const int16_t *pcm, int samples, const uint8_t *ima) {
    if (!pcm && !ima) { app_voice_capture_failed(); return; }
    if (s_ending || samples != MIC_FRAME_SAMPLES) return;
    if (app_voice_live_active() && audio_mic_dropped_ms() != s_input_dropped_ms) { app_voice_capture_failed(); return; }
    portENTER_CRITICAL(&s_input_mux);
    uint8_t turn = s_input_turn;
    uint32_t session = s_input_session;
    portEXIT_CRITICAL(&s_input_mux);
    uint32_t epoch = audio_capture_epoch();
    if (!send_input(pcm, samples, turn, session, ima)) {
        s_ending = true;
        if (audio_mic_request_stop_epoch(epoch))
            app_post_in_session(EV_INPUT_END, turn | (2 << 8), (int32_t)(epoch + 1), session);
        return;
    }
    if (app_voice_mode() == VOICE_LIVE || !s_vad) return;
    check_pause(pcm, samples, turn, session, epoch);
}

bool app_voice_start_capture(void) {
    if (app_voice_mode() == VOICE_CLASSIC) {
        if (!audio_mic_gate(true, classic_frame)) { audio_mic_gate(false, NULL); return false; }
        app_voice_capture_started(s_turn);
        return true;
    }
    audio_mic_gate(false, NULL);
    app_voice_recording_stop();
    portENTER_CRITICAL(&s_input_mux);
    bool ended = s_queued_end;
    portEXIT_CRITICAL(&s_input_mux);
    if (ended) { app_voice_capture_started(s_turn); return true; }
    if (app_voice_mode() == VOICE_REALTIME) {
        s_vad = vad_create_with_param(VAD_MODE_3, 16000, 20, 20, 20);
        if (!s_vad) return false;
    }
    audio_mic_set_duplex(app_voice_mode() == VOICE_LIVE);
    s_pause = (auto_voice_t){0}; s_resample = (wake_resample_t){0}; s_ending = false;
    s_input_dropped_ms = audio_mic_dropped_ms();
    if (!audio_mic_gate(true, native_frame)) {
        audio_mic_gate(false, NULL); app_voice_recording_stop(); return false;
    }
    app_voice_capture_started(s_turn);
    return true;
}

void app_voice_recording_stop(void) {
    // App task calls this only after the capture callback has drained.
    if (s_input_frames) ESP_LOGI("voice", "input frames=%u pcm=%ums elapsed=%ums send=%ums max=%ums over40=%u dropped=%ums",
        (unsigned)s_input_frames, (unsigned)(s_input_frames * 40), (unsigned)(now_ms() - s_input_started_ms),
        (unsigned)s_send_ms, (unsigned)s_send_max_ms, (unsigned)s_send_slow,
        (unsigned)(audio_mic_dropped_ms() - s_input_dropped_ms));
    s_input_frames = s_send_ms = s_send_max_ms = s_send_slow = 0;
    if (s_vad) { vad_destroy(s_vad); s_vad = NULL; }
    s_ending = false;
    audio_mic_set_duplex(false);
}

static void receive_live_input(cJSON *json) {
    cJSON *turn = cJSON_GetObjectItem(json, "turn"), *on = cJSON_GetObjectItem(json, "on");
    if (cJSON_IsNumber(turn) && turn->valuedouble == turn->valueint && turn->valueint >= 0 &&
        turn->valueint <= 255 && cJSON_IsBool(on))
        app_post_in_session(EV_INPUT_END, turn->valueint | ((cJSON_IsTrue(on) ? 4 : 3) << 8), 0, link_session());
}

static void cancel_output(cJSON *json) {
    cJSON *gen = cJSON_GetObjectItem(json, "gen");
    if (cJSON_IsNumber(gen) && gen->valuedouble == gen->valueint && gen->valueint > 0 && gen->valueint <= 255)
        app_post_in_session(EV_SRV_SPEAK_CANCEL, gen->valueint, 0, link_session());
}

static void apply_capabilities(cJSON *json) {
        bool stt, tts;
        if (!agent_protocol_parse_capabilities(json, &stt, &tts)) return;
        const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(json, "voice_mode"));
        int value = mode && !strcmp(mode, "live") ? VOICE_LIVE :
                    mode && !strcmp(mode, "realtime") ? VOICE_REALTIME : VOICE_CLASSIC;
        ESP_LOGI("voice", "capabilities mode=%d stt=%d tts=%d", value, stt, tts);
        app_post_in_session(EV_AGENT_CAPS, stt, (value << 1) | tts, link_session());
}

bool app_voice_receive(cJSON *json, const char *type) {
    if (!strcmp(type, "speak_cancel")) { cancel_output(json); return true; }
    if (!strcmp(type, "capabilities")) {
        apply_capabilities(json); return true;
    }
    if (!strcmp(type, "live_input")) { receive_live_input(json); return true; }
    if (strcmp(type, "input_end")) return false;
    cJSON *turn = cJSON_GetObjectItem(json, "turn");
    if (cJSON_IsNumber(turn) && turn->valuedouble >= 0 && turn->valuedouble <= 255 &&
        turn->valuedouble == turn->valueint) request_end((uint8_t)turn->valueint, link_session());
    return true;
}

static bool pause_native_input(bool live) {
    bool closed = audio_mic_gate(false, NULL);
    app_wake_recording_stop();
    if (!closed) { talk_fire(TE_ABORT); return false; }
    if (!live && talk_is_listening(s_talk)) {
        talk_fire(s_talk == TALK_HOLD ? TE_RELEASE_LONG : TE_LATCH_TIMEOUT);
        s_await_t0 = now_ms(); s_think_cues = 0;
    }
    return true;
}

static void live_input(int turn, bool on, uint32_t session) {
    if (turn != s_input_turn || session != s_input_session || session != link_session()) return;
    bool live = app_voice_live_active();
    if (!live && (app_voice_mode() != VOICE_REALTIME || on)) return;
    if (on) {
        if (!audio_mic_is_open() && !app_voice_start_capture()) {
            app_voice_live_stop(true);
            face_lock(); face_card(&g_face, "Could not resume the microphone. Try KEY again."); face_unlock();
            return;
        }
    } else if (!pause_native_input(live)) return;
    if (live) atomic_store(&s_live_ready, true);
    char ack[64]; snprintf(ack, sizeof ack, "{\"t\":\"live_input_ack\",\"turn\":%d,\"on\":%s}", turn, on ? "true" : "false");
    link_send_json_in_session(ack, session);
    disp_wake();
}

static void end_live_input(int turn, int reason, int capture_epoch, uint32_t session) {
    if (turn != s_input_turn || session != s_input_session || session != link_session()) return;
    if (reason == 2 && (uint32_t)capture_epoch != audio_capture_epoch()) return;
    app_voice_live_stop(reason == 2);
}

void app_voice_input_end(int packed, int capture_epoch, uint32_t session) {
    int turn = packed & 255, reason = (unsigned)packed >> 8;
    if (reason >= 3) { if (reason <= 4) live_input(turn, reason == 4, session); return; }
    if (app_voice_live_active()) { end_live_input(turn, reason, capture_epoch, session); return; }
    if (session != link_session() || turn != s_turn || (uint32_t)capture_epoch != audio_capture_epoch() || !talk_is_listening(s_talk)) return;
    if (reason == 1) { finish_turn(s_talk == TALK_HOLD ? TE_RELEASE_LONG : TE_LATCH_TIMEOUT); return; }
    audio_mic_gate(false, NULL);
    app_wake_recording_stop();
    if (reason == 2) {
        char message[48]; snprintf(message, sizeof message, "{\"t\":\"cancel\",\"turn\":%d}", turn);
        link_send_json_in_session(message, session);
        talk_fire(TE_ABORT); bubble(BUB_STOP, STR_NOT_HEARD, 2.f); audio_sfx(SFX_NOT_HEARD);
        return;
    }
    talk_fire(s_talk == TALK_HOLD ? TE_RELEASE_LONG : TE_LATCH_TIMEOUT);
    s_await_t0 = now_ms(); s_think_cues = 0;
    disp_wake();
}

void app_voice_capabilities(int stt, int flags) {
    if (app_voice_live_active() && (app_voice_mode() != (voice_mode_t)(flags >> 1) || !stt)) app_voice_live_stop(true);
    if (s_vad && app_voice_mode() != (voice_mode_t)(flags >> 1)) {
        audio_mic_gate(false, NULL); app_voice_recording_stop(); talk_fire(TE_ABORT);
    }
    atomic_store(&s_mode, flags >> 1);
    app_agent_set_capabilities(stt != 0, (flags & 1) != 0);
}
