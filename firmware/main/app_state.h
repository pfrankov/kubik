// The two explicit state machines of the app: how the screen is powered and where a talk turn is.
// Pure tables (no platform calls) so the host tests can walk every state x event pair.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DIM_MS 45000     // no interaction: the screen dims
#define SLEEP_MS 180000  // ... and Kubik falls asleep
#define DARKEN_MS 15000  // the sleep fade to black
#define DIM_PERCENT 30

// ------------------------------------------------------------------ screen power
typedef enum {
    PWR_AWAKE,        // lit, the user's brightness
    PWR_DIMMED,       // lit at DIM_PERCENT, still interactive
    PWR_SLEEPING,     // asleep face, brightness fading to black
    PWR_DARK_MANUAL,  // PWR pressed: panel off, Kubik not asleep
    PWR_DARK,         // asleep and panel off
    PWR_GOODBYE,      // powering off: asleep face, fading, the PMIC cuts the power next
    PWR_STATE_COUNT
} power_state_t;

typedef enum {
    PE_NONE = -1,
    PE_WAKE,        // interaction or status while the screen is visible
    PE_IDLE_DIM,
    PE_IDLE_SLEEP,
    PE_IDLE_DARK,
    PE_BUTTON,      // short PWR press: toggles the screen
    PE_GOODBYE,     // PWR held 2 s
    PE_DARK_NOW,    // lost Wi-Fi given up: dark at once
    PE_REPLY,       // actual agent reply may wake a fully dark screen
    PE_COUNT
} power_event_t;

enum {  // what the transition asks the display to do, in this order
    PA_SCREEN_ON = 1 << 0,
    PA_SCREEN_OFF = 1 << 1,
    PA_BRIGHT_NOW = 1 << 2,  // back to full brightness at once
    PA_FADE_UP = 1 << 3,
    PA_FADE_DIM = 1 << 4,
    PA_FADE_SLEEP = 1 << 5,
    PA_FADE_GOODBYE = 1 << 6,
    PA_WAKE_FACE = 1 << 7,   // the character wakes up
};

typedef struct {
    power_state_t next;
    uint8_t actions;
    bool legal;
} power_step_t;

power_step_t power_step(power_state_t state, power_event_t event);
power_event_t power_idle_event(power_state_t state, int64_t idle_ms);
int power_brightness(power_state_t state, int user);
bool power_is_dark(power_state_t state);    // panel off
bool power_is_asleep(power_state_t state);  // the character sleeps (the face shows it)
bool power_is_active(power_state_t state);  // lit and interactive

// ------------------------------------------------------------------ talk turn
typedef enum {
    TALK_IDLE,
    TALK_HOLD,      // capture gate open while the key is held
    TALK_LATCHED,   // quick press: recording until the next press or tap
    TALK_AWAITING,  // turn sent, waiting for the reply
    TALK_STATE_COUNT
} talk_state_t;

typedef enum {
    TE_PRESS,
    TE_RELEASE_SHORT,  // released before LATCH_MS
    TE_RELEASE_LONG,
    TE_TAP,            // touch while latched
    TE_LATCH_TIMEOUT,
    TE_RESOLVE,        // reply, error, server idle or the wait ran out
    TE_ABORT,          // link lost or powering off
    TE_COUNT
} talk_event_t;

// The state after `event` (the same state when the event means nothing in it).
talk_state_t talk_next(talk_state_t state, talk_event_t event);
bool talk_is_listening(talk_state_t state);

// ------------------------------------------------------------------ light sleep gate
// ESP32-C6 light sleep switches the USB Serial/JTAG pad off: the host loses the port. Sleep is allowed only
// while the screen is dark and idle, the PMIC reported "no VBUS" on LIGHT_SLEEP_QUIET_READS polls in a row
// (a failed read counts as VBUS present), and no USB host has spoken lately. Radio/check activity keeps the CPU awake. Zero reads: no sleep.
#define LIGHT_SLEEP_QUIET_READS 2
int usb_gate_read(int quiet_reads, bool vbus);  // the count after one PMIC poll
bool light_sleep_allowed(bool doze, int quiet_reads, bool host_active, bool radio_active);
