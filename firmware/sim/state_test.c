// Host test of the screen-power and talk-turn tables: every state x event pair, illegal ones included.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"

// Expected next state per event, '.' = the event means nothing here (the state must not change).
// Power columns: WAKE IDLE_DIM IDLE_SLEEP IDLE_DARK BUTTON GOODBYE DARK_NOW REPLY; letters: A awake, D dimmed,
// S sleeping, M dark manual, K dark, G goodbye.
static const char *const k_power_expect[PWR_STATE_COUNT] = {
    [PWR_AWAKE] = "ADS.MGKA",
    [PWR_DIMMED] = "A.S.M.KA",
    [PWR_SLEEPING] = "A..KK.KA",
    [PWR_DARK_MANUAL] = "..K.AGKA",
    [PWR_DARK] = "....AGKA",
    [PWR_GOODBYE] = "........",
};
static const power_state_t k_letter_state[] = {['A'] = PWR_AWAKE, ['D'] = PWR_DIMMED, ['S'] = PWR_SLEEPING,
                                                ['M'] = PWR_DARK_MANUAL, ['K'] = PWR_DARK, ['G'] = PWR_GOODBYE};
// Talk columns: PRESS REL_SHORT REL_LONG TAP LATCH_TMO RESOLVE ABORT; letters: I idle, H hold, L latched, W awaiting.
static const char *const k_talk_expect[TALK_STATE_COUNT] = {
    [TALK_IDLE] = "H......",
    [TALK_HOLD] = "HLW...I",
    [TALK_LATCHED] = "W..WW.I",
    [TALK_AWAITING] = "H....II",
};
static const talk_state_t k_talk_letter[] = {['I'] = TALK_IDLE, ['H'] = TALK_HOLD, ['L'] = TALK_LATCHED,
                                             ['W'] = TALK_AWAITING};

static void check_power_table(void) {
    for (int s = 0; s < PWR_STATE_COUNT; s++) {
        for (int e = 0; e < PE_COUNT; e++) {
            char want = k_power_expect[s][e];
            power_step_t step = power_step((power_state_t)s, (power_event_t)e);
            assert(step.legal == (want != '.'));
            assert(step.next == (want == '.' ? (power_state_t)s : k_letter_state[(int)want]));
            if (!step.legal) assert(step.actions == 0);
        }
    }
}

// What the display is asked to do must agree with the panel state the two ends imply.
static void check_power_actions(void) {
    for (int s = 0; s < PWR_STATE_COUNT; s++) {
        for (int e = 0; e < PE_COUNT; e++) {
            power_step_t step = power_step((power_state_t)s, (power_event_t)e);
            bool was_dark = power_is_dark((power_state_t)s), now_dark = power_is_dark(step.next);
            bool dark_shutdown = was_dark && step.next == PWR_GOODBYE;
            assert(!!(step.actions & PA_SCREEN_ON) == (was_dark && !now_dark && !dark_shutdown));
            assert(!!(step.actions & PA_SCREEN_OFF) == (!was_dark && now_dark));
            if (step.actions & PA_WAKE_FACE) assert(power_is_asleep((power_state_t)s) && !power_is_asleep(step.next));
            if (!power_is_asleep((power_state_t)s) && power_is_asleep(step.next) && step.next != PWR_DARK && !dark_shutdown)
                assert(step.actions & (PA_FADE_SLEEP | PA_FADE_GOODBYE));
        }
    }
}

// Every state but the last one can get back to AWAKE by waking; GOODBYE ignores everything.
static void check_power_reachability(void) {
    bool seen[PWR_STATE_COUNT] = {true};
    for (int pass = 0; pass < PWR_STATE_COUNT; pass++) {
        for (int s = 0; s < PWR_STATE_COUNT; s++) {
            for (int e = 0; seen[s] && e < PE_COUNT; e++) seen[power_step((power_state_t)s, (power_event_t)e).next] = true;
        }
    }
    for (int s = 0; s < PWR_STATE_COUNT; s++) assert(seen[s]);
    for (int s = 0; s < PWR_GOODBYE; s++) assert(power_step((power_state_t)s, PE_REPLY).next == PWR_AWAKE);
}

static void check_idle_timeline(void) {
    assert(power_idle_event(PWR_AWAKE, DIM_MS) == PE_NONE);
    assert(power_idle_event(PWR_AWAKE, DIM_MS + 1) == PE_IDLE_DIM);
    assert(power_idle_event(PWR_AWAKE, SLEEP_MS + 1) == PE_IDLE_SLEEP);
    assert(power_idle_event(PWR_DIMMED, DIM_MS + 1) == PE_NONE);
    assert(power_idle_event(PWR_DIMMED, SLEEP_MS + 1) == PE_IDLE_SLEEP);
    assert(power_idle_event(PWR_DARK_MANUAL, DIM_MS + 1) == PE_NONE);
    assert(power_idle_event(PWR_DARK_MANUAL, SLEEP_MS + 1) == PE_IDLE_SLEEP);
    assert(power_idle_event(PWR_SLEEPING, SLEEP_MS + 1) == PE_NONE);
    assert(power_idle_event(PWR_SLEEPING, SLEEP_MS + DARKEN_MS + 501) == PE_IDLE_DARK);
    assert(power_idle_event(PWR_DARK, 100 * SLEEP_MS) == PE_NONE);
    assert(power_idle_event(PWR_GOODBYE, 100 * SLEEP_MS) == PE_NONE);
    // Walking a long idle stretch reaches the same end however the ticks fall.
    power_state_t state = PWR_AWAKE;
    for (int64_t idle = 0; idle < 400000; idle += 50) {
        power_event_t event = power_idle_event(state, idle);
        if (event != PE_NONE) state = power_step(state, event).next;
        assert(idle <= DIM_MS || state != PWR_AWAKE);
    }
    assert(state == PWR_DARK);
}

static void check_brightness(void) {
    assert(power_brightness(PWR_AWAKE, 200) == 200);
    assert(power_brightness(PWR_DIMMED, 200) == 60);
    assert(power_brightness(PWR_DIMMED, 255) == 76);
    assert(power_brightness(PWR_DIMMED, 10) == 8);  // never black while lit
    assert(power_brightness(PWR_DIMMED, 5) == 5);
    assert(power_brightness(PWR_DARK_MANUAL, 200) == 200);
    assert(power_brightness(PWR_SLEEPING, 200) == 0 && power_brightness(PWR_DARK, 200) == 0);
    assert(power_brightness(PWR_GOODBYE, 200) == 0);
}

static void check_talk_table(void) {
    for (int s = 0; s < TALK_STATE_COUNT; s++) {
        for (int e = 0; e < TE_COUNT; e++) {
            char want = k_talk_expect[s][e];
            assert(talk_next((talk_state_t)s, (talk_event_t)e) ==
                   (want == '.' ? (talk_state_t)s : k_talk_letter[(int)want]));
        }
    }
}

// A whole turn, and the paths that must always come back to idle.
static void check_talk_scenarios(void) {
    talk_state_t s = TALK_IDLE;
    s = talk_next(s, TE_PRESS); assert(s == TALK_HOLD && talk_is_listening(s));
    s = talk_next(s, TE_RELEASE_LONG); assert(s == TALK_AWAITING && !talk_is_listening(s));
    s = talk_next(s, TE_RESOLVE); assert(s == TALK_IDLE);
    s = talk_next(s, TE_PRESS);
    s = talk_next(s, TE_RELEASE_SHORT); assert(s == TALK_LATCHED && talk_is_listening(s));
    s = talk_next(s, TE_PRESS); assert(s == TALK_AWAITING);  // the second press sends
    s = talk_next(s, TE_PRESS); assert(s == TALK_HOLD);      // talking again while waiting
    s = talk_next(s, TE_PRESS); assert(s == TALK_HOLD);      // a lost release must not wedge the key
    for (int start = 0; start < TALK_STATE_COUNT; start++)
        assert(talk_next((talk_state_t)start, TE_ABORT) == TALK_IDLE);
}

// Light sleep needs a dark idle screen, VBUS absent on consecutive polls, and a silent host; any doubt keeps it off.
static void check_light_sleep_gate(void) {
    int reads = 0;  // boot: the PMIC has not been read yet
    assert(!light_sleep_allowed(true, reads, false, false));
    reads = usb_gate_read(reads, false);
    assert(!light_sleep_allowed(true, reads, false, false));  // one clean poll is not enough
    reads = usb_gate_read(reads, false);
    assert(light_sleep_allowed(true, reads, false, false));
    assert(!light_sleep_allowed(false, reads, false, false));  // lit or busy
    assert(!light_sleep_allowed(true, reads, false, true)); // active Wi-Fi never loses the CPU
    assert(!light_sleep_allowed(true, reads, true, false));    // a bridge is talking
    for (int i = 0; i < 100; i++) reads = usb_gate_read(reads, false);
    assert(reads == LIGHT_SLEEP_QUIET_READS);
    reads = usb_gate_read(reads, true);  // VBUS appears (or the PMIC read failed): off at once, count restarts
    assert(reads == 0 && !light_sleep_allowed(true, reads, false, false));
    reads = usb_gate_read(reads, false);
    assert(!light_sleep_allowed(true, reads, false, false));
    // No sequence that contains VBUS in its last poll may allow sleep.
    for (int mask = 0; mask < 256; mask++) {
        reads = 0;
        for (int b = 0; b < 8; b++) reads = usb_gate_read(reads, (mask >> b) & 1);
        assert(light_sleep_allowed(true, reads, false, false) == ((mask & 0xC0) == 0));
    }
}

int main(void) {
    check_power_table();
    check_power_actions();
    check_power_reachability();
    check_idle_timeline();
    check_brightness();
    check_talk_table();
    check_talk_scenarios();
    check_light_sleep_gate();
    puts("state tables ok");
    return 0;
}
