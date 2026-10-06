#include "app_state.h"

// ------------------------------------------------------------------ screen power
#define STEP(to, acts) {.next = (to), .actions = (acts), .legal = true}
#define LIT_UP (PA_FADE_UP | PA_WAKE_FACE)

static const power_step_t k_power[PWR_STATE_COUNT][PE_COUNT] = {
    [PWR_AWAKE] = {
        [PE_WAKE] = STEP(PWR_AWAKE, 0),
        [PE_REPLY] = STEP(PWR_AWAKE, 0),
        [PE_IDLE_DIM] = STEP(PWR_DIMMED, PA_FADE_DIM),
        [PE_IDLE_SLEEP] = STEP(PWR_SLEEPING, PA_FADE_SLEEP),
        [PE_BUTTON] = STEP(PWR_DARK_MANUAL, PA_SCREEN_OFF),
        [PE_GOODBYE] = STEP(PWR_GOODBYE, PA_FADE_GOODBYE),
        [PE_DARK_NOW] = STEP(PWR_DARK, PA_SCREEN_OFF),
    },
    [PWR_DIMMED] = {
        [PE_WAKE] = STEP(PWR_AWAKE, PA_BRIGHT_NOW),
        [PE_REPLY] = STEP(PWR_AWAKE, PA_BRIGHT_NOW),
        [PE_IDLE_SLEEP] = STEP(PWR_SLEEPING, PA_FADE_SLEEP),
        [PE_BUTTON] = STEP(PWR_DARK_MANUAL, PA_BRIGHT_NOW | PA_SCREEN_OFF),
        [PE_DARK_NOW] = STEP(PWR_DARK, PA_BRIGHT_NOW | PA_SCREEN_OFF),
    },
    [PWR_SLEEPING] = {
        [PE_WAKE] = STEP(PWR_AWAKE, LIT_UP),
        [PE_REPLY] = STEP(PWR_AWAKE, LIT_UP),
        [PE_IDLE_DARK] = STEP(PWR_DARK, PA_SCREEN_OFF),
        [PE_BUTTON] = STEP(PWR_DARK, PA_SCREEN_OFF),
        [PE_DARK_NOW] = STEP(PWR_DARK, PA_SCREEN_OFF),
    },
    [PWR_DARK_MANUAL] = {
        [PE_GOODBYE] = STEP(PWR_GOODBYE, 0),
        [PE_REPLY] = STEP(PWR_AWAKE, PA_SCREEN_ON),
        [PE_IDLE_SLEEP] = STEP(PWR_DARK, PA_FADE_SLEEP),  // the fade runs unseen: the next wake glides up
        [PE_BUTTON] = STEP(PWR_AWAKE, PA_SCREEN_ON),
        [PE_DARK_NOW] = STEP(PWR_DARK, 0),
    },
    [PWR_DARK] = {
        [PE_GOODBYE] = STEP(PWR_GOODBYE, 0),
        [PE_REPLY] = STEP(PWR_AWAKE, PA_SCREEN_ON | LIT_UP),
        [PE_BUTTON] = STEP(PWR_AWAKE, PA_SCREEN_ON | LIT_UP),
        [PE_DARK_NOW] = STEP(PWR_DARK, 0),
    },
    [PWR_GOODBYE] = {{0}},  // only the PMIC ends it
};

power_step_t power_step(power_state_t state, power_event_t event) {
    power_step_t step = k_power[state][event];
    if (!step.legal) step.next = state;
    return step;
}

power_event_t power_idle_event(power_state_t state, int64_t idle_ms) {
    if (state == PWR_SLEEPING) return idle_ms > SLEEP_MS + DARKEN_MS + 500 ? PE_IDLE_DARK : PE_NONE;
    if (state != PWR_AWAKE && state != PWR_DIMMED && state != PWR_DARK_MANUAL) return PE_NONE;
    if (idle_ms > SLEEP_MS) return PE_IDLE_SLEEP;
    return state == PWR_AWAKE && idle_ms > DIM_MS ? PE_IDLE_DIM : PE_NONE;
}

// The panel level for a state, given the user's brightness setting (0-255).
int power_brightness(power_state_t state, int user) {
    if (state == PWR_SLEEPING || state == PWR_DARK || state == PWR_GOODBYE) return 0;
    if (state != PWR_DIMMED) return user;
    int dimmed = user * DIM_PERCENT / 100;
    return dimmed < 8 ? (user < 8 ? user : 8) : dimmed;
}

bool power_is_dark(power_state_t state) { return state == PWR_DARK_MANUAL || state == PWR_DARK; }
bool power_is_asleep(power_state_t state) {
    return state == PWR_SLEEPING || state == PWR_DARK || state == PWR_GOODBYE;
}
bool power_is_active(power_state_t state) { return state == PWR_AWAKE || state == PWR_DIMMED; }

// ------------------------------------------------------------------ talk turn
#define X -1  // means nothing in this state

static const int8_t k_talk[TALK_STATE_COUNT][TE_COUNT] = {
    //             PRESS          REL_SHORT     REL_LONG        TAP             LATCH_TMO       RESOLVE     ABORT
    [TALK_IDLE]     = {TALK_HOLD, X,            X,              X,              X,              X,          X},
    [TALK_HOLD]     = {TALK_HOLD, TALK_LATCHED, TALK_AWAITING,  X,              X,              X,          TALK_IDLE},
    [TALK_LATCHED]  = {TALK_AWAITING, X,        X,              TALK_AWAITING,  TALK_AWAITING,  X,          TALK_IDLE},
    [TALK_AWAITING] = {TALK_HOLD, X,            X,              X,              X,              TALK_IDLE,  TALK_IDLE},
};

talk_state_t talk_next(talk_state_t state, talk_event_t event) {
    int next = k_talk[state][event];
    return next < 0 ? state : (talk_state_t)next;
}

bool talk_is_listening(talk_state_t state) { return state == TALK_HOLD || state == TALK_LATCHED; }

// ------------------------------------------------------------------ light sleep gate
int usb_gate_read(int quiet_reads, bool vbus) {
    if (vbus) return 0;
    return quiet_reads < LIGHT_SLEEP_QUIET_READS ? quiet_reads + 1 : quiet_reads;
}

bool light_sleep_allowed(bool doze, int quiet_reads, bool host_active, bool radio_active) {
    return doze && quiet_reads >= LIGHT_SLEEP_QUIET_READS && !host_active && !radio_active;
}
