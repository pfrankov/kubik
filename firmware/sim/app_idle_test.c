// Actual runtime idle decisions, with only external device services stubbed.
#include <assert.h>
#include <stdio.h>
#include "app_state.h"

static bool live, stream, s_setup, s_menu;
static struct { int card_n, cron_running; float t, cron_due; } g_face;
typedef struct { int a, b; } app_ev_t;
#define ESP_LOGI(...) ((void)0)
static void face_lock(void) {}
static void face_unlock(void) {}
static int s_act_own, s_gen = -1;
static char s_pair_code[17];
static talk_state_t s_talk;
static power_state_t s_power;
static int64_t s_debug_until, s_last_activity, clock_ms;
static bool app_voice_live_active(void) { return live; }
static bool audio_stream_playing(void) { return stream; }
static void power_fire(power_event_t event, bool sound) {
    assert(!sound);  // all automatic transitions are silent
    power_step_t step = power_step(s_power, event);
    assert(step.legal);
    s_power = step.next;
}
static void wake(bool sound) {
    if (power_is_dark(s_power)) return;
    s_last_activity = clock_ms;
    power_fire(PE_WAKE, sound);
}
static void app_journal_event(const app_ev_t *e) { (void)e; }
/* PRODUCTION_POLICY */

static void tick(int64_t t) { clock_ms = t; tick_sleep(t); }
static void idle_sequence(void) {
    s_power = PWR_AWAKE;
    s_last_activity = 0;
    tick(DIM_MS + 1);
    assert(s_power == PWR_DIMMED);
    tick(SLEEP_MS + 1);
    assert(s_power == PWR_SLEEPING);
    tick(SLEEP_MS + DARKEN_MS + 501);
    assert(s_power == PWR_DARK);
}

int main(void) {
    idle_sequence();
    // Repeated and newly started background jobs never wake or reset idle.
    const int64_t last = s_last_activity;
    for (int running = 0; running < 3; running++) {
        app_ev_t event = {.a = running, .b = -1};
        handle_cron_event(&event);
        assert(s_power == PWR_DARK && s_last_activity == last);
        assert(g_face.cron_running == running && g_face.cron_due == -1);
    }
    // Every active operation inhibits sleep; its completion starts a fresh idle interval.
    for (int blocker = 0; blocker < 10; blocker++) {
        live = blocker == 0;
        s_talk = blocker == 1 ? TALK_LATCHED : blocker == 2 ? TALK_AWAITING : TALK_IDLE;
        s_act_own = blocker == 3;
        s_gen = blocker == 4 ? 7 : -1;
        stream = blocker == 5;
        s_setup = blocker == 6;
        s_pair_code[0] = blocker == 7 ? 'X' : 0;
        s_menu = blocker == 8;
        g_face.card_n = blocker == 9;
        s_power = PWR_DIMMED;
        tick(500000);
        assert(s_power == PWR_AWAKE && s_last_activity == 500000);
        live = stream = s_setup = s_menu = false;
        s_talk = TALK_IDLE; s_gen = -1; s_act_own = 0; s_pair_code[0] = 0;
        g_face.card_n = 0;
        tick(500000 + DIM_MS + 1);
        assert(s_power == PWR_DIMMED);
        tick(500000 + SLEEP_MS + 1);
        assert(s_power == PWR_SLEEPING);
    }
    puts("runtime idle: silent sleep, active operations inhibit, completion releases");
}
