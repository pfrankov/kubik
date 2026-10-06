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
        EV_TOUCH_DOWN, EV_VOICE_WAKE, EV_SETUP_START};
    for (int state = 0; state < PWR_STATE_COUNT; state++) {
        s_power = state;
        for (unsigned i = 0; i < sizeof blocked / sizeof *blocked; i++) {
            calls = 0; app_ev_t e = {.type = blocked[i]}; app_handle(&e);
            assert((calls == 0) == power_is_dark(s_power));
        }
    }
    s_power = PWR_DARK;
    const app_ev_type_t allowed[] = {EV_PWR_SHORT, EV_PWR_LONG, EV_LINK_UP, EV_LINK_DOWN,
        EV_SRV_TEXT, EV_SRV_SPEAK, EV_SRV_WELCOME, EV_SRV_CRON, EV_SRV_ACTIVITY, EV_SRV_STATE,
        EV_INPUT_END, EV_VOICE_END, EV_AGENT_CAPS};
    for (unsigned i = 0; i < sizeof allowed / sizeof *allowed; i++) {
        calls = 0; app_ev_t e = {.type = allowed[i]}; app_handle(&e); assert(calls);
    }
    calls = 0; app_ev_t reboot = {.type = EV_BOOT_LONG, .a = 1};
    app_handle(&reboot); assert(calls);
    s_power = PWR_DIMMED; clock_ms = 1000;
    app_ev_t contact = {.type = EV_TOUCH_DOWN}; app_handle(&contact);
    assert(s_power == PWR_AWAKE && s_last_activity == clock_ms);
    clock_ms += 200; app_handle(&contact);
    assert(s_last_activity == clock_ms && s_last_touch_or_key == clock_ms);
    puts("dark input: all user gestures/BOOT/KEY ignored; PWR, replies and background processing retained");
}
