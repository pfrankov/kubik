// Tess's mood: a small state machine that turns what happened to it (touches, shakes, sounds, what the agent said, the
// state of the link and the battery) into one lasting mood, and lets the mood fade back to CALM when nothing happens.
// Pure C without face_t, like rub.c: the caller (tess_feel.c) gathers the events, reads the blend weights, plays the cue.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { MOOD_CALM, MOOD_CURIOUS, MOOD_PLAYFUL, MOOD_LOVED, MOOD_SCARED, MOOD_GRUMPY, MOOD_SAD, MOOD_SLEEPY, MOOD_COUNT } mood_t;

// What happened, one bit each (mood_input_t.events). ME_PESTER is made by the machine itself, from taps and shakes piling up.
typedef enum {
    ME_TOUCH, ME_TAP, ME_PET, ME_RUB1, ME_RUB3, ME_HEART, ME_GLAD,   // touch: a finger lands, taps, pets, rubs (stage 1-2, 3+), the heart, a game won
    ME_SHAKE, ME_JOLT, ME_PICKUP,                                   // movement and vibration
    ME_TALK, ME_ANSWER, ME_FAIL,                                     // the voice turn: the person speaks, the answer, nothing heard / it failed
    ME_SAY_JOY, ME_SAY_LOVE, ME_SAY_SAD, ME_SAY_ANGRY, ME_SAY_SURPRISE,  // the agent's [[tags]]
    ME_IGNORED, ME_LINK_LOST, ME_LINK_BACK, ME_WAKE,
    ME_PESTER,
    ME_COUNT
} mood_event_t;
#define ME_BIT(event) (1u << (event))
#define MOOD_VIA_DECAY ME_COUNT       // mood_machine_t.via: it faded here on its own
#define MOOD_VIA_TIRED (ME_COUNT + 1) // ...or it got tired / rested, no event

// Whether the mood may run: LIVE it runs, TALK (a voice turn) freezes the clocks, HELD (thinking, offline, sleep) too.
typedef enum { MOOD_LIVE, MOOD_TALK, MOOD_HELD } mood_gate_t;

typedef struct {
    uint32_t events;      // ME_BIT()s since the last step
    float event_strength[ME_COUNT];  // 0..1 how hard each event was; only its own event can set a mood's level
    float event_side[ME_COUNT];      // -1..1 where each event came from; kept with it through a batch
    bool drowsy, battery_low, charging;  // what the app says
    bool touching;        // a finger is on it: the mood is kept
    mood_gate_t gate;
} mood_input_t;

typedef struct {
    uint8_t now, prev;    // the mood, and the one it came from
    uint8_t cause;        // the event (or MOOD_VIA_*) that brought it, and its rank
    uint8_t cause_rank;
    float cause_strength, cause_side; // the selected event's payload, retained after the input batch is cleared
    uint8_t stage;        // 1: it faded here from a stronger mood, so it is held for half the time
    bool entered;         // this step changed the mood
    bool pinned;          // (tests, the simulator): the mood does not fade
    float in_s, quiet_s;  // seconds in it (always), seconds of nothing (LIVE and no finger only)
    float tired_s;        // seconds CALM while drowsy or the battery is low
    float pester;         // taps and shakes piling up
    float level, peak, intensity;  // how strongly it was entered (.5..1), the highest since it began, and now (falls across the hold)
    float prev_peak;      // `peak` of the mood it came from
    float progress;       // 0..1 of the cross-fade to `now`
    float from[MOOD_COUNT], weights[MOOD_COUNT];  // the blend when it began, and the blend now (sums to 1)
} mood_machine_t;

void mood_reset(mood_machine_t *machine);
// Advances by dt seconds; true when the mood changed (machine->entered, ->prev, ->cause, ->level say how).
bool mood_step(mood_machine_t *machine, const mood_input_t *input, float dt);
float mood_hold_s(mood_t mood);      // how long it lasts once nothing more happens (0: as long as its cause)
float mood_fade_s(mood_t mood);      // how long it takes to blend in
const char *mood_name(mood_t mood);  // "calm", "curious", ...
int mood_next(mood_t from, mood_event_t event);  // the table: the mood the event leads to (the same one: it is kept up), -1 if nothing
