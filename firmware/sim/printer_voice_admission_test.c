// Execute production PTT admission and printer/Live snapshot gates.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "app_state.h"
#include "printer_platform.h"
#define KUBIK_VOICE_BUTTON "KEY"
#define ESP_LOGI(...) ((void)0)
#define SFX_TAP 1
#define SFX_ERROR 2
#define SFX_LISTEN_START 3
#define EMO_SAD 1
#define SS_IDLE 0
#define pdMS_TO_TICKS(ms) (ms)
static bool admitted, live, s_ignore_up, s_setup, s_online, s_menu, busy, muse, s_act_own;
static char s_pair_code[17];
static int s_power_off_at, s_gen = -1, s_srv, g_face, s_turn;
static int suspends, cards, sounds, allocations, captures, sends, stops;
static int64_t s_last_touch_or_key, s_ptt_t0;
static talk_state_t s_talk;
static bool printer_protocol_before_voice(void) { return admitted; }
static void app_wake_suspend(void) { suspends++; }
static int64_t now_ms(void) { return 100; }
static void wake(bool sound) { assert(!sound); }
static bool muse_pair_key(void) { return false; }
static bool app_voice_live_active(void) { return live; }
static void app_voice_live_stop(bool manual) { assert(manual); live = false; stops++; }
static void audio_sfx(int sound) { (void)sound; sounds++; }
static void menu_close(bool sound) { assert(!sound); s_menu = false; }
static void finish_turn(talk_event_t event) { (void)event; }
static void face_lock(void) {}
static void face_unlock(void) {}
static void face_card(int *face, const char *text) { (void)face; if(text) cards++; }
static void tess_games_end_sky(int *face) { (void)face; }
static void offline_bubble(float duration) { (void)duration; }
static void face_emo(int emotion, float duration) { (void)emotion; (void)duration; }
static bool app_agent_mic_allowed(void) { return true; }
static void app_agent_reclaim_catalog(void) { allocations++; }
static bool tls_mem_turn(bool wifi) { (void)wifi; allocations++; return true; }
static const char *link_via(void) { return "usb"; }
static void stop_speech(bool clear) { assert(clear); }
static void app_voice_prepare_capture(int turn) { (void)turn; allocations++; }
static void talk_fire(talk_event_t event) { s_talk = event == TE_PRESS ? TALK_HOLD : TALK_IDLE; }
static void disp_wake(void) {}
#define send_json(...) ((void)sends++)
static void wifi_set_fast(bool fast) { assert(fast); }
static void vTaskDelay(int ticks) { assert(ticks == 140); }
static bool app_voice_start_capture(void) { captures++; return true; }
/* PRODUCTION_PTT_DOWN */
static bool audio_stream_busy(void) { return busy; }
static bool muse_backend_selected(void) { return muse; }
static uint32_t link_session(void) { return 42; }
static void printer_status_set_visible(bool visible) { (void)visible; }
static bool app_printer_visible(void) { return s_menu; }
/* PRODUCTION_PRINTER_STATE */
static void denied_voice_has_no_allocations(void) {
    s_online = true;
    ptt_down();
    assert(s_ignore_up && cards == 1 && !suspends && !sounds && !allocations);
    assert(!captures && !sends && !s_turn && s_talk == TALK_IDLE);
}
static void admitted_voice_and_live_stop(void) {
    admitted = true; s_ignore_up = false; ptt_down();
    assert(suspends == 1 && captures == 1 && sends == 1 && s_turn == 1);
    assert(allocations == 3 && s_talk == TALK_HOLD);
    live = true; ptt_down();
    assert(stops == 1 && !live && captures == 1 && allocations == 3);
}
static void live_allows_foreground_only(void) {
    printer_app_state_t state;
    s_talk = TALK_IDLE; printer_app_state(&state);
    assert(state.can_start && state.automatic_allowed && state.online && state.session == 42);
    live = true; s_talk = TALK_HOLD; printer_app_state(&state);
    assert(state.can_start && !state.automatic_allowed && live);
    assert(state.blockers==768);
    busy = true; printer_app_state(&state); assert(!state.can_start && live);
    assert(state.blockers==896);
    busy = false; live = false; printer_app_state(&state); assert(!state.can_start);
    s_talk = TALK_IDLE; muse = true; printer_app_state(&state);
    assert(!state.enabled && !state.can_start && !state.automatic_allowed);
}
int main(void) {
    denied_voice_has_no_allocations();
    admitted_voice_and_live_stop();
    live_allows_foreground_only();
    puts("printer voice admission: delayed cleanup blocks new PTT; Live input and stop remain intact");
}
