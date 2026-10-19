// Run the production dispatch gate: dark input is discarded before any handler.
#include <assert.h>
#include <stdio.h>
#include "app_state.h"
/* PRODUCTION_EVENTS */
typedef struct { app_ev_type_t type; int a, b; uint32_t session; } app_ev_t;
static power_state_t s_power;
static bool s_power_off_failed;
static int s_power_off_at, calls;
static bool app_lab_event(const app_ev_t *e) { (void)e; calls++; return false; }
static void esp_restart(void) { calls++; }
static void app_wake_event(uint32_t a) { (void)a; calls++; }
static void app_wake_end_event(int a, int b) { (void)a; (void)b; calls++; }
static void app_voice_input_end(int a, int b, uint32_t s) { (void)a; (void)b; (void)s; calls++; }
static bool handle_control_event(const app_ev_t *e) { (void)e; calls++; return false; }
static bool s_setup, s_pair_hidden, s_menu;
static char s_pair_code[17];
static int64_t s_last_touch_or_key, s_last_activity, clock_ms;
static int64_t now_ms(void) { return clock_ms; }
static void wake(bool sound) {
    assert(!sound); power_step_t step = power_step(s_power, PE_WAKE);
    if (step.legal) { s_power = step.next; s_last_activity = clock_ms; }
}
static void power_fire(power_event_t event, bool sound) {
    assert(!sound); power_step_t step = power_step(s_power, event); s_power = step.next; s_last_activity = clock_ms;
}
static void menu_tap(int x, int y) { (void)x; (void)y; calls++; }
static void app_touch_event(const app_ev_t *e) { (void)e; calls++; }
/* PRODUCTION_TOUCH */
static bool handle_link_event(const app_ev_t *e) { (void)e; calls++; return false; }
static void handle_agent_capabilities(const app_ev_t *e) { (void)e; calls++; }
static bool handle_server_event(const app_ev_t *e) { (void)e; calls++; return false; }
/* PRODUCTION_DISPATCH */
int main(void) {
    const app_ev_type_t blocked[] = {EV_PTT_DOWN, EV_PTT_UP, EV_BOOT_SHORT, EV_BOOT_LONG,
        EV_TAP, EV_PET, EV_SHAKE, EV_PICKUP, EV_SWIPE_UP, EV_DRAG, EV_MENU_HOLD,
        EV_TOUCH_DOWN, EV_VOICE_WAKE, EV_SETUP_START, EV_MOTION_ACTIVITY};
    for (int state = 0; state < PWR_STATE_COUNT; state++) {
        s_power = state;
        for (unsigned i = 0; i < sizeof blocked / sizeof *blocked; i++) {
            s_power = state; s_touch_wake_swallow = false; clock_ms += 10000;
            calls = 0; app_ev_t e = {.type = blocked[i]}; app_handle(&e);
            assert((calls == 0) == power_is_dark((power_state_t)state));
        }
    }
    // The screen that went dark by itself wakes to a touch or a lift, silently; the button's dark never does.
    const app_ev_type_t wakers[] = {EV_TOUCH_DOWN, EV_PICKUP};
    for (unsigned i = 0; i < 2; i++) {
        s_power = PWR_DARK_MANUAL; clock_ms += 10000; calls = 0;
        app_ev_t e = {.type = wakers[i]}; app_handle(&e);
        assert(s_power == PWR_DARK_MANUAL && calls == 0);
        s_power = PWR_DARK; clock_ms += 10000; calls = 0; app_handle(&e);
        assert(s_power == PWR_AWAKE && calls == 0);  // the touch or lift only wakes
    }
    s_power = PWR_DARK; clock_ms += 10000; app_ev_t down = {.type = EV_TOUCH_DOWN, .a = 1}; app_handle(&down);
    app_ev_t tap = {.type = EV_TAP}; calls = 0; clock_ms += 300; app_handle(&tap);
    assert(calls == 0);                              // the rest of the waking gesture is swallowed
    clock_ms += 5000; down.a = 0; app_handle(&down); app_handle(&tap); assert(calls == 0);
    app_ev_t pet = {.type = EV_PET}; app_handle(&pet); assert(calls == 0);
    app_ev_t up = {.type = EV_TOUCH_UP}; app_handle(&up); assert(calls == 0);
    down.a = 1; app_handle(&down); app_handle(&tap); assert(calls > 0);  // a new contact acts normally
    s_power = PWR_DARK;
    const app_ev_type_t allowed[] = {EV_PWR_SHORT, EV_PWR_LONG, EV_LINK_UP, EV_LINK_DOWN,
        EV_SRV_TEXT, EV_SRV_SPEAK, EV_SRV_WELCOME, EV_SRV_CRON, EV_SRV_ACTIVITY, EV_SRV_STATE,
        EV_INPUT_END, EV_VOICE_END, EV_AGENT_CAPS};
    for (unsigned i = 0; i < sizeof allowed / sizeof *allowed; i++) {
        calls = 0; app_ev_t e = {.type = allowed[i]}; app_handle(&e); assert(calls);
    }
    calls = 0; app_ev_t reboot = {.type = EV_BOOT_LONG, .a = 1};
    app_handle(&reboot); assert(calls);
    s_power = PWR_DIMMED; clock_ms += 10000;
    app_ev_t contact = {.type = EV_TOUCH_DOWN}; app_handle(&contact);
    assert(s_power == PWR_AWAKE && s_last_activity == clock_ms);
    clock_ms += 200; app_handle(&contact);
    assert(s_last_activity == clock_ms && s_last_touch_or_key == clock_ms);
    for (int state=PWR_AWAKE; state<=PWR_SLEEPING; state++) {
        s_power=state; clock_ms+=10000;
        int64_t touch_at=s_last_touch_or_key;
        app_ev_t motion={.type=EV_MOTION_ACTIVITY}; app_handle(&motion);
        assert(s_power==PWR_AWAKE && s_last_activity==clock_ms && s_last_touch_or_key==touch_at);
    }
    s_power=PWR_DIMMED; s_menu=true; clock_ms+=10000;
    app_ev_t pickup={.type=EV_PICKUP}; app_handle(&pickup);
    assert(s_power==PWR_AWAKE && s_last_activity==clock_ms); // menu / recent touch never swallow activity
    puts("dark input: all user gestures/BOOT/KEY ignored; PWR, replies and background processing retained");
}
