#include "mood.h"

#include <string.h>

// Where an event leads from each mood. 0 = nothing; else the mood plus one. The same mood again refreshes it.
#define TO(mood) ((uint8_t)((mood) + 1))
#define E(event) [event]
#define EVERYTHING_PLAYFUL E(ME_RUB1) = TO(MOOD_PLAYFUL), E(ME_RUB3) = TO(MOOD_PLAYFUL), E(ME_GLAD) = TO(MOOD_PLAYFUL), E(ME_SAY_JOY) = TO(MOOD_PLAYFUL)
// The events that mean a mood in every mood that does not care: startle, love, anger.
#define ALARM E(ME_SHAKE) = TO(MOOD_SCARED)
#define AFFECTION E(ME_HEART) = TO(MOOD_LOVED), E(ME_SAY_LOVE) = TO(MOOD_LOVED)
#define ANNOYANCE E(ME_PESTER) = TO(MOOD_GRUMPY), E(ME_SAY_ANGRY) = TO(MOOD_GRUMPY)
#define WOE E(ME_SAY_SAD) = TO(MOOD_SAD), E(ME_LINK_LOST) = TO(MOOD_SAD)

static const uint8_t k_next[MOOD_COUNT][ME_COUNT] = {
    [MOOD_CALM] = {
        E(ME_TOUCH) = TO(MOOD_CURIOUS), E(ME_TAP) = TO(MOOD_CURIOUS), E(ME_JOLT) = TO(MOOD_CURIOUS), E(ME_PICKUP) = TO(MOOD_CURIOUS),
        E(ME_WAKE) = TO(MOOD_CURIOUS), E(ME_SAY_SURPRISE) = TO(MOOD_CURIOUS),
        E(ME_RUB3) = TO(MOOD_PLAYFUL), E(ME_GLAD) = TO(MOOD_PLAYFUL), E(ME_SAY_JOY) = TO(MOOD_PLAYFUL), E(ME_LINK_BACK) = TO(MOOD_PLAYFUL),
        ALARM, AFFECTION, ANNOYANCE, WOE,
        E(ME_FAIL) = TO(MOOD_SAD), E(ME_IGNORED) = TO(MOOD_SAD),
    },
    [MOOD_CURIOUS] = {
        E(ME_TOUCH) = TO(MOOD_CURIOUS), E(ME_TAP) = TO(MOOD_CURIOUS), E(ME_JOLT) = TO(MOOD_CURIOUS), E(ME_PICKUP) = TO(MOOD_CURIOUS),
        E(ME_SAY_SURPRISE) = TO(MOOD_CURIOUS),
        E(ME_RUB3) = TO(MOOD_PLAYFUL), E(ME_GLAD) = TO(MOOD_PLAYFUL), E(ME_SAY_JOY) = TO(MOOD_PLAYFUL), E(ME_LINK_BACK) = TO(MOOD_PLAYFUL),
        ALARM, AFFECTION, ANNOYANCE, WOE,
        E(ME_FAIL) = TO(MOOD_SAD), E(ME_IGNORED) = TO(MOOD_SAD),
    },
    [MOOD_PLAYFUL] = {  // tolerant: taps and rubs only keep it going; the pester threshold is doubled
        E(ME_TOUCH) = TO(MOOD_PLAYFUL), E(ME_TAP) = TO(MOOD_PLAYFUL), E(ME_PET) = TO(MOOD_PLAYFUL), EVERYTHING_PLAYFUL,
        ALARM, AFFECTION, ANNOYANCE, WOE,
        E(ME_FAIL) = TO(MOOD_SAD), E(ME_IGNORED) = TO(MOOD_SAD),
    },
    [MOOD_LOVED] = {
        E(ME_TOUCH) = TO(MOOD_LOVED), E(ME_TAP) = TO(MOOD_LOVED), E(ME_PET) = TO(MOOD_LOVED), E(ME_RUB1) = TO(MOOD_LOVED),
        E(ME_RUB3) = TO(MOOD_LOVED), E(ME_GLAD) = TO(MOOD_LOVED), E(ME_HEART) = TO(MOOD_LOVED), E(ME_SAY_LOVE) = TO(MOOD_LOVED),
        ALARM, ANNOYANCE, WOE,
        E(ME_FAIL) = TO(MOOD_SAD),
    },
    [MOOD_SCARED] = {  // soothed by kindness
        E(ME_PET) = TO(MOOD_CALM), E(ME_RUB1) = TO(MOOD_CALM), E(ME_RUB3) = TO(MOOD_CALM), E(ME_GLAD) = TO(MOOD_CALM),
        E(ME_SAY_JOY) = TO(MOOD_CALM), E(ME_TALK) = TO(MOOD_CURIOUS), E(ME_ANSWER) = TO(MOOD_CALM),
        E(ME_SHAKE) = TO(MOOD_SCARED), E(ME_HEART) = TO(MOOD_LOVED), E(ME_SAY_LOVE) = TO(MOOD_LOVED),
        ANNOYANCE, WOE,
    },
    [MOOD_GRUMPY] = {  // keeps its mood under taps and shakes, and grows more so
        E(ME_TAP) = TO(MOOD_GRUMPY), E(ME_SHAKE) = TO(MOOD_GRUMPY), E(ME_PESTER) = TO(MOOD_GRUMPY), E(ME_SAY_ANGRY) = TO(MOOD_GRUMPY),
        E(ME_PET) = TO(MOOD_CALM), E(ME_RUB1) = TO(MOOD_CALM), E(ME_GLAD) = TO(MOOD_CALM), E(ME_SAY_JOY) = TO(MOOD_CALM),
        E(ME_RUB3) = TO(MOOD_PLAYFUL), AFFECTION, WOE,
    },
    [MOOD_SAD] = {  // comforted by company
        E(ME_TOUCH) = TO(MOOD_CALM), E(ME_PET) = TO(MOOD_CALM), E(ME_TALK) = TO(MOOD_CALM), E(ME_ANSWER) = TO(MOOD_CALM),
        E(ME_SAY_JOY) = TO(MOOD_CALM), E(ME_GLAD) = TO(MOOD_PLAYFUL), E(ME_RUB3) = TO(MOOD_PLAYFUL), E(ME_LINK_BACK) = TO(MOOD_PLAYFUL),
        ALARM, AFFECTION, ANNOYANCE,
    },
    [MOOD_SLEEPY] = {  // woken by anything
        E(ME_TOUCH) = TO(MOOD_CURIOUS), E(ME_TAP) = TO(MOOD_CURIOUS), E(ME_TALK) = TO(MOOD_CURIOUS), E(ME_WAKE) = TO(MOOD_CURIOUS),
        E(ME_JOLT) = TO(MOOD_CURIOUS), E(ME_PICKUP) = TO(MOOD_CURIOUS),
        E(ME_RUB1) = TO(MOOD_CALM), E(ME_RUB3) = TO(MOOD_PLAYFUL), E(ME_GLAD) = TO(MOOD_PLAYFUL), E(ME_SAY_JOY) = TO(MOOD_PLAYFUL),
        ALARM, AFFECTION, ANNOYANCE,
    },
};

// What outweighs what: a stronger event may end a mood before its minimum dwell has passed.
static const uint8_t k_rank[ME_COUNT] = {
    [ME_PESTER] = 10, [ME_SHAKE] = 9, [ME_HEART] = 8,
    [ME_SAY_JOY] = 7, [ME_SAY_LOVE] = 7, [ME_SAY_SAD] = 7, [ME_SAY_ANGRY] = 7, [ME_SAY_SURPRISE] = 7, [ME_FAIL] = 7,
    [ME_LINK_LOST] = 6, [ME_LINK_BACK] = 6, [ME_IGNORED] = 6,
    [ME_PET] = 5, [ME_RUB1] = 5, [ME_RUB3] = 5, [ME_GLAD] = 4, [ME_TAP] = 3, [ME_TOUCH] = 3,
    [ME_JOLT] = 2, [ME_PICKUP] = 2, [ME_WAKE] = 2, [ME_TALK] = 1, [ME_ANSWER] = 1,
};
// Per mood: the shortest time before another mood may take over (except by a stronger event), how long it lasts once
// nothing more happens, where it fades to, and how long it takes to blend in.
static const float k_dwell[MOOD_COUNT] = {0, 2.f, 3.f, 4.f, 1.5f, 4.f, 4.f, 5.f};
static const float k_hold[MOOD_COUNT] = {0, 8.f, 12.f, 20.f, 4.f, 15.f, 30.f, 0};
static const uint8_t k_decay_to[MOOD_COUNT] = {MOOD_CALM, MOOD_CALM, MOOD_CALM, MOOD_PLAYFUL, MOOD_CURIOUS, MOOD_CALM, MOOD_CALM, MOOD_CALM};
static const float k_fade[MOOD_COUNT] = {.8f, .5f, .6f, 1.f, .4f, .7f, 1.f, 1.f};

#define TIRED_S 5.f            // s CALM while drowsy or worn out before it nods off
#define PESTER_TAP 1.f
#define PESTER_SHAKE 4.f
#define PESTER_LIMIT 6.f       // taps and shakes (weighted) that make it cross; doubled where it is tolerant
#define PESTER_DECAY (1.f / 4) // per second
#define LEVEL_ENTER .5f        // how strongly a mood starts: this plus half the event's strength
#define LEVEL_REFRESH .3f
#define LEVEL_FADED .6f        // a mood that faded from a stronger one

int mood_next(mood_t from, mood_event_t event) { return (int)k_next[from][event] - 1; }
float mood_hold_s(mood_t mood) { return k_hold[mood]; }
float mood_fade_s(mood_t mood) { return k_fade[mood]; }

void mood_reset(mood_machine_t *m) {
    memset(m, 0, sizeof *m);
    m->cause = MOOD_VIA_DECAY;
    m->level = m->peak = m->intensity = 1.f;
    m->progress = 1.f;
    m->from[MOOD_CALM] = m->weights[MOOD_CALM] = 1.f;
    m->in_s = 99.f;
}

static void begin(mood_machine_t *m, mood_t to, int via, int rank, float level, float strength, float side, bool faded) {
    if (to == m->now) {  // again: it lasts longer and is felt more
        m->quiet_s = 0;
        m->stage = 0;
        m->level = m->level + LEVEL_REFRESH > 1.f ? 1.f : m->level + LEVEL_REFRESH;
        if (m->level > m->peak) m->peak = m->level;
        if (rank > m->cause_rank) m->cause_rank = (uint8_t)rank;
        return;
    }
    memcpy(m->from, m->weights, sizeof m->from);
    m->pinned = false;  // (a mood held by a test lets go when an event changes it)
    m->progress = 0;
    m->prev_peak = m->peak;
    m->prev = m->now;
    m->now = (uint8_t)to;
    m->cause = (uint8_t)via;
    m->cause_rank = (uint8_t)rank;
    m->cause_strength = via < ME_COUNT ? strength : 0.f;
    m->cause_side = via < ME_COUNT ? side : 0.f;
    m->stage = faded;
    m->in_s = m->quiet_s = 0;
    m->level = m->peak = level;
    m->entered = true;
}

// The highest-ranked event of this step that leads somewhere and is not held back by the dwell guard.
static int pick_event(const mood_machine_t *m, uint32_t events) {
    int best = -1;
    for (int e = 0; e < ME_COUNT; e++) {
        if (!(events & ME_BIT(e)) || !k_next[m->now][e]) continue;
        bool change = k_next[m->now][e] - 1 != m->now;
        if (change && m->in_s < k_dwell[m->now] && k_rank[e] <= m->cause_rank) continue;
        if (best < 0 || k_rank[e] > k_rank[best]) best = e;
    }
    return best;
}

// Taps and shakes pile up; too many at once is too much (a pester).
static uint32_t add_pester(mood_machine_t *m, const mood_input_t *in, float dt, float *pester_strength) {
    uint32_t events = in->events;
    *pester_strength = 0.f;
    m->pester -= dt * PESTER_DECAY;
    if (m->pester < 0) m->pester = 0;
    if (events & ME_BIT(ME_TAP)) {
        m->pester += PESTER_TAP;
        *pester_strength = in->event_strength[ME_TAP];
    }
    if (events & ME_BIT(ME_SHAKE)) {
        m->pester += PESTER_SHAKE;
        if (in->event_strength[ME_SHAKE] > *pester_strength) *pester_strength = in->event_strength[ME_SHAKE];
    }
    bool tolerant = m->now == MOOD_PLAYFUL || m->now == MOOD_LOVED;
    if (m->pester < PESTER_LIMIT * (tolerant ? 2.f : 1.f)) return events;
    m->pester *= .5f;
    return events | ME_BIT(ME_PESTER);
}

static float hold_now(const mood_machine_t *m) { return k_hold[m->now] * (m->stage ? .5f : 1.f); }

// Nothing more happened for its hold: it fades to the next, milder mood; SLEEPY goes when the reason for it is gone.
static bool fade_on(mood_machine_t *m, bool tired) {
    if (m->now == MOOD_SLEEPY) {
        if (tired || m->pinned || m->in_s < k_dwell[MOOD_SLEEPY]) return false;
        begin(m, MOOD_CALM, MOOD_VIA_TIRED, 0, 1.f, 0.f, 0.f, false);
        return true;
    }
    float hold = hold_now(m);
    if (hold <= 0 || m->quiet_s < hold) return false;
    mood_t to = (mood_t)k_decay_to[m->now];
    begin(m, to, MOOD_VIA_DECAY, 0, to == MOOD_CALM ? 1.f : LEVEL_FADED, 0.f, 0.f, true);
    return true;
}

static void blend(mood_machine_t *m, float dt) {
    m->progress += dt / k_fade[m->now];
    if (m->progress >= 1.f) {
        m->progress = 1.f;
        memset(m->weights, 0, sizeof m->weights);
        m->weights[m->now] = 1.f;
        return;
    }
    float s = m->progress * m->progress * (3.f - 2.f * m->progress);
    for (int i = 0; i < MOOD_COUNT; i++) m->weights[i] = m->from[i] * (1.f - s) + (i == m->now ? s : 0.f);
}

// How strongly a mood is entered: from LEVEL_ENTER for a light touch to 1 for the hardest shake.
static float entry_level(float strength) { return LEVEL_ENTER + .5f * (strength < 0 ? 0 : strength > 1 ? 1 : strength); }

// The mood stays on its own clock: time in it, time of nothing, and time drowsy while calm.
static void tick(mood_machine_t *m, const mood_input_t *in, bool tired, float dt) {
    bool live = in->gate == MOOD_LIVE;
    m->in_s += dt;
    if (live && !in->touching && !m->pinned) m->quiet_s += dt;
    m->tired_s = live && tired && m->now == MOOD_CALM ? m->tired_s + dt : 0;
}

bool mood_step(mood_machine_t *m, const mood_input_t *in, float dt) {
    bool tired = in->drowsy || (in->battery_low && !in->charging);
    m->entered = false;
    float pester_strength;
    uint32_t events = add_pester(m, in, dt, &pester_strength);
    tick(m, in, tired, dt);
    int e = pick_event(m, events);
    if (e >= 0) {
        float strength = e == ME_PESTER && !(in->events & ME_BIT(ME_PESTER)) ? pester_strength : in->event_strength[e];
        float side = e == ME_PESTER && !(in->events & ME_BIT(ME_PESTER)) ? 0.f : in->event_side[e];
        begin(m, (mood_t)(k_next[m->now][e] - 1), e, k_rank[e], entry_level(strength), strength, side, false);
    } else if (!fade_on(m, tired) && m->tired_s >= TIRED_S) begin(m, MOOD_SLEEPY, MOOD_VIA_TIRED, 0, 1.f, 0.f, 0.f, false);
    float hold = hold_now(m);
    m->intensity = hold > 0 ? m->level * (1.f - (m->quiet_s < hold ? m->quiet_s / hold : 1.f)) : m->level;
    blend(m, dt);
    return m->entered;
}

const char *mood_name(mood_t mood) {
    static const char *const k_name[MOOD_COUNT] = {"calm", "curious", "playful", "loved", "scared", "grumpy", "sad", "sleepy"};
    return (unsigned)mood < MOOD_COUNT ? k_name[mood] : "?";
}
