// Production remote event handlers: only a current, substantive reply wakes darkness.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_state.h"
#include "face.h"
#define ESP_LOGI(...) ((void)0)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef enum { SS_IDLE, SS_THINKING } srv_state_t;
enum { SFX_NOTIFY };
typedef struct { int a, b; uint32_t session; } app_ev_t;
static int s_srv, s_act_own, s_act_other, s_gen = -1, s_text_mux;
static int64_t s_await_t0, s_last_activity;
static char s_text_rx[128];
static uint32_t s_text_receipt, s_text_session = 1, s_text_revision = 1;
static bool s_text_notify;
static face_t g_face;
static int wakes, cards;
static power_state_t s_power;
static int64_t now_ms(void) { return 1000; }
static uint32_t link_session(void) { return 1; }
static void wake(bool sound) {
    assert(!sound);
    power_step_t step = power_step(s_power, PE_WAKE);
    if (step.legal) { s_power = step.next; wakes++; }
}
static void wake_for_reply(void) {
    power_step_t step = power_step(s_power, PE_REPLY);
    assert(step.legal); s_power = step.next; wakes++;
}
static void talk_fire(int event) { assert(event == TE_RESOLVE); }
static void app_journal_event(const app_ev_t *e) { (void)e; }
static bool app_speech_matches(int gen, uint32_t session) { return gen == 7 && session == 1; }
static void audio_sfx(int sound) { assert(sound == SFX_NOTIFY); }
static bool audio_stream_playing(void) { return false; }
static void face_ev(int event, int a, int b) { (void)event; (void)a; (void)b; }
static void face_lock(void) {}
static void face_unlock(void) {}
void face_card(face_t *f, const char *text) { assert(f == &g_face); cards += text[0] != 0; }
static void app_journal_text(const char *text, bool notify) { (void)text; (void)notify; }
static void link_send_json_in_session(const char *text, uint32_t session) { assert(text[0] && session == 1); }
/* PRODUCTION_REPLIES */
int main(void) {
    app_ev_t e = {.a=SS_THINKING, .b=1, .session=1};
    s_power = PWR_DARK; handle_state_event(&e); handle_activity_event(&e);
    assert(s_power == PWR_DARK && !wakes); // processing continues without waking
    s_power = PWR_DIMMED; handle_state_event(&e); assert(s_power == PWR_AWAKE);
    e.a = 1; // current text revision
    const char *empty[] = {"", " \t\r\n"};
    for (unsigned i = 0; i < sizeof empty / sizeof *empty; i++) {
        strcpy(s_text_rx, empty[i]); s_power = PWR_DARK; wakes = 0;
        handle_text_event(&e); assert(s_power == PWR_DARK && !wakes);
    }
    strcpy(s_text_rx, "Here is your reply.");
    e.session = 2; handle_text_event(&e); assert(s_power == PWR_DARK);
    e.session = 1; e.a = 2; handle_text_event(&e); assert(s_power == PWR_DARK);
    e.a = 1; handle_text_event(&e); assert(s_power == PWR_AWAKE && cards == 2);
    s_text_notify = true; s_power = PWR_DARK; handle_text_event(&e); assert(s_power == PWR_AWAKE);
    s_power = PWR_DARK; e.a = 7; e.session = 2;
    handle_speak_event(&e); assert(s_power == PWR_DARK);
    e.session = 1; e.b = 0; handle_speak_event(&e); assert(s_power == PWR_AWAKE);
    puts("reply wake: status/activity stay dark; empty/stale text ignored; current text and voice wake");
}
