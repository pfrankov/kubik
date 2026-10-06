// Host test of Tess's mood machine (mood.c): the whole event table, the priority and dwell rules, staged decay back to
// CALM at steady and jittery frame times, the gates, the pester escalation, the blend weights. tools/test-state.py
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/mood.h"

#define DT (1.f / 30)
// Where each event leads from each mood ('.' nowhere): C calm, U curious, P playful, L loved, S scared, G grumpy, D sad, Z sleepy.
// Columns: TOUCH TAP PET RUB1 RUB3 HEART GLAD SHAKE JOLT PICKUP TALK ANSWER FAIL SAY_JOY SAY_LOVE SAY_SAD SAY_ANGRY
// SAY_SURPRISE IGNORED LINK_LOST LINK_BACK WAKE PESTER.
static const char *const k_expect[MOOD_COUNT] = {
    [MOOD_CALM] = "UU..PLPSUU..DPLDGUDDPUG",
    [MOOD_CURIOUS] = "UU..PLPSUU..DPLDGUDDP.G",
    [MOOD_PLAYFUL] = "PPPPPLPS....DPLDG.DD..G",
    [MOOD_LOVED] = "LLLLLLLS....D.LDG..D..G",
    [MOOD_SCARED] = "..CCCLCS..UC.CLDG..D..G",
    [MOOD_GRUMPY] = ".GCCPLCG.....CLDG..D..G",
    [MOOD_SAD] = "C.C.PLPS..CC.CL.G...P.G",
    [MOOD_SLEEPY] = "UU.CPLPSUUU..PL.G....UG",
};
static const char k_letters[] = "CUPLSGDZ";
static const char *const k_names[MOOD_COUNT] = {"calm", "curious", "playful", "loved", "scared", "grumpy", "sad", "sleepy"};

static mood_machine_t fresh(void) {
    mood_machine_t m;
    mood_reset(&m);
    return m;
}

static mood_input_t event_input(uint32_t events, float strength, float side, mood_gate_t gate) {
    mood_input_t in = {.events = events, .gate = gate};
    for (int e = 0; e < ME_COUNT; e++)
        if (events & ME_BIT(e)) { in.event_strength[e] = strength; in.event_side[e] = side; }
    return in;
}

// Steps `seconds` at dt with `events` in the first step only.
static bool run(mood_machine_t *m, uint32_t events, float seconds, mood_gate_t gate) {
    mood_input_t in = event_input(events, .5f, 0.f, gate);
    bool entered = false;
    for (float t = 0; t < seconds; t += DT) {
        entered |= mood_step(m, &in, DT);
        in.events = 0;
    }
    return entered;
}

static void check_table(void) {
    for (int from = 0; from < MOOD_COUNT; from++) {
        assert(strlen(k_expect[from]) == ME_COUNT && ME_COUNT == 23);
        for (int e = 0; e < ME_COUNT; e++) {
            int to = mood_next((mood_t)from, (mood_event_t)e);
            char want = k_expect[from][e];
            assert(want == '.' ? to == -1 : (to >= 0 && k_letters[to] == want));
        }
    }
}

// From CALM every mood can be reached, and every mood leads back to CALM by itself.
static void check_reach(void) {
    bool seen[MOOD_COUNT] = {true, [MOOD_SLEEPY] = true};  // (SLEEPY comes of tiredness, not an event)
    for (int pass = 0; pass < MOOD_COUNT; pass++)
        for (int from = 0; from < MOOD_COUNT; from++)
            for (int e = 0; e < ME_COUNT; e++) {
                int to = mood_next((mood_t)from, (mood_event_t)e);
                if (seen[from] && to >= 0) seen[to] = true;
            }
    for (int i = 0; i < MOOD_COUNT; i++) assert(seen[i]);
    for (int i = MOOD_CURIOUS; i < MOOD_COUNT; i++) {  // its fade ends in CALM (SLEEPY's, once nothing makes it tired)
        mood_machine_t m = fresh();
        m.now = (uint8_t)i;
        m.weights[i] = 1;
        m.weights[MOOD_CALM] = 0;
        run(&m, 0, 40.f, MOOD_LIVE);
        assert(m.now == MOOD_CALM);
    }
}

static const uint32_t k_cause[MOOD_COUNT] = {0, ME_BIT(ME_TAP), ME_BIT(ME_GLAD), ME_BIT(ME_HEART), ME_BIT(ME_SHAKE), ME_BIT(ME_SAY_ANGRY), ME_BIT(ME_FAIL)};

// How long each mood takes to go all the way back to CALM once nothing more happens, at any frame time.
static void check_decay_times(void) {
    static const float k_within[] = {0, 8.5f, 12.5f, 27.f, 9.f, 16.f, 31.f};
    for (int jitter = 0; jitter < 2; jitter++)
        for (int i = MOOD_CURIOUS; i < MOOD_SLEEPY; i++) {
            mood_machine_t m = fresh();
            mood_input_t in = event_input(k_cause[i], 1.f, 0.f, MOOD_LIVE);
            unsigned seed = 7;
            float t = 0, back = -1;
            for (int n = 0; n < 60 * 30 && back < 0; n++) {
                seed = seed * 1664525u + 1013904223u;
                float dt = jitter ? .027f + .013f * ((seed >> 16) & 255) / 255.f : DT;
                mood_step(&m, &in, dt);
                in.events = 0;
                if (n == 0) assert(m.now == i && m.entered);
                if (m.now == MOOD_CALM) back = t;
                t += dt;
            }
            assert(back > 0 && back <= k_within[i]);
            if (i == MOOD_LOVED || i == MOOD_SCARED) assert(back > mood_hold_s((mood_t)i));  // through the milder mood
            printf("%s: back to calm after %.1f s%s\n", k_names[i], back, jitter ? " (jittery frames)" : "");
        }
}

static void random_storm_input(mood_input_t *in, unsigned *seed) {
    static const mood_event_t events[] = {ME_TAP, ME_SHAKE, ME_JOLT, ME_SAY_SAD, ME_LINK_LOST, ME_HEART, ME_PET, ME_WAKE};
    in->events = 0;
    memset(in->event_strength, 0, sizeof in->event_strength);
    memset(in->event_side, 0, sizeof in->event_side);
    for (size_t i = 0; i < sizeof events / sizeof events[0]; i++) {
        *seed = *seed * 1664525u + 1013904223u;
        if ((*seed >> 24) < 12) {
            int e = events[i];
            in->events |= ME_BIT(e);
            in->event_strength[e] = ((*seed >> 4) & 255) / 255.f;
            in->event_side[e] = ((*seed >> 12) & 255) / 127.5f - 1.f;
        }
    }
    *seed = *seed * 1664525u + 1013904223u;
    in->gate = (mood_gate_t)((*seed >> 12) % 7 == 0 ? (*seed >> 16) % 3 : MOOD_LIVE);
}

static void check_cause_payload(const mood_machine_t *m, const mood_input_t *in) {
    if (!m->entered || m->cause >= ME_COUNT) return;
    if (m->cause == ME_PESTER && !(in->events & ME_BIT(ME_PESTER))) {
        float want_strength = 0.f;
        if (in->events & ME_BIT(ME_TAP)) want_strength = in->event_strength[ME_TAP];
        if ((in->events & ME_BIT(ME_SHAKE)) && in->event_strength[ME_SHAKE] > want_strength)
            want_strength = in->event_strength[ME_SHAKE];
        assert(m->cause_strength == want_strength && m->cause_side == 0.f);
        return;
    }
    assert(m->cause_strength == in->event_strength[m->cause] && m->cause_side == in->event_side[m->cause]);
}

static void check_weights(const mood_machine_t *m) {
    float sum = 0;
    for (int i = 0; i < MOOD_COUNT; i++) {
        assert(m->weights[i] >= 0 && m->weights[i] <= 1.0001f);
        sum += m->weights[i];
    }
    assert(fabsf(sum - 1.f) < 1e-4f);
}

static void run_random_trial(int trial, unsigned *seed) {
    mood_machine_t m = fresh();
    mood_input_t in = {.gate = MOOD_LIVE};
    int steps = 1 + trial % 900;
    for (int n = 0; n < steps; n++) {
        random_storm_input(&in, seed);
        mood_step(&m, &in, DT);
        check_cause_payload(&m, &in);
        check_weights(&m);
    }
    run(&m, 0, 60.f, MOOD_LIVE);
    assert(m.now == MOOD_CALM && m.weights[MOOD_CALM] == 1.f);
}

// Random pokes for a while, then silence: always CALM within 60 s, with the blend one-hot again.
static void check_always_calm(void) {
    unsigned seed = 12345;
    for (int trial = 0; trial < 1000; trial++) run_random_trial(trial, &seed);
}

// With no input events, time alone never invents a room-sound or attention event.
static void check_empty_input_stays_calm(void) {
    mood_machine_t m = fresh();
    mood_input_t in = {.gate = MOOD_LIVE};
    for (int i = 0; i < 120 * 30; i++) mood_step(&m, &in, DT);
    assert(m.now == MOOD_CALM && m.weights[MOOD_CALM] == 1.f && in.events == 0);
}

// The clocks run only while LIVE and untouched; the state is kept, and events are still taken.
static void check_gates(void) {
    for (mood_gate_t gate = MOOD_TALK; gate <= MOOD_HELD; gate++) {
        mood_machine_t m = fresh();
        run(&m, ME_BIT(ME_SAY_SAD), .1f, MOOD_LIVE);
        assert(m.now == MOOD_SAD);
        run(&m, 0, 120.f, gate);
        assert(m.now == MOOD_SAD && m.quiet_s < .2f);
        run(&m, ME_BIT(ME_SHAKE), .1f, gate);
        assert(m.now == MOOD_SCARED);
        run(&m, 0, 5.f, MOOD_LIVE);
        assert(m.now != MOOD_SCARED);  // its hold runs only now
    }
    mood_machine_t m = fresh();
    run(&m, ME_BIT(ME_SAY_SAD), .1f, MOOD_LIVE);
    mood_input_t held = {.touching = true, .gate = MOOD_LIVE};
    for (int i = 0; i < 100 * 30; i++) mood_step(&m, &held, DT);
    assert(m.now == MOOD_SAD);  // a finger keeps the mood
}

// Priority: within a step the strongest event wins; inside the dwell only a stronger one preempts.
static void check_priority_and_dwell(void) {
    mood_machine_t m = fresh();
    mood_input_t batch = event_input(ME_BIT(ME_TAP) | ME_BIT(ME_SHAKE) | ME_BIT(ME_SAY_SAD), .5f, .2f, MOOD_LIVE);
    batch.event_strength[ME_TAP] = .15f; batch.event_side[ME_TAP] = .8f;
    batch.event_strength[ME_SAY_SAD] = .9f; batch.event_side[ME_SAY_SAD] = -.9f;
    batch.event_strength[ME_SHAKE] = .35f; batch.event_side[ME_SHAKE] = -.25f;
    mood_step(&m, &batch, DT);
    assert(m.now == MOOD_SCARED && m.cause == ME_SHAKE);  // shake over tag over tap
    assert(fabsf(m.level - .675f) < 1e-5f && m.cause_strength == .35f && m.cause_side == -.25f);
    m = fresh();
    run(&m, ME_BIT(ME_HEART), .5f, MOOD_LIVE);
    assert(m.now == MOOD_LOVED);
    float heart_strength = m.cause_strength, heart_side = m.cause_side;
    mood_input_t cooldown = event_input(ME_BIT(ME_LINK_LOST), 1.f, -.9f, MOOD_LIVE);
    mood_step(&m, &cooldown, DT);  // rank 6 cannot replace the heart's rank 8 within the dwell
    assert(m.now == MOOD_LOVED && m.cause == ME_HEART && m.cause_strength == heart_strength && m.cause_side == heart_side);
    assert(!run(&m, ME_BIT(ME_TAP), 1.f, MOOD_LIVE) && m.now == MOOD_LOVED);            // a tap keeps it (refresh)
    m = fresh();
    run(&m, ME_BIT(ME_SAY_SAD), .5f, MOOD_LIVE);
    assert(m.now == MOOD_SAD);
    run(&m, ME_BIT(ME_PET), .5f, MOOD_LIVE);           // inside SAD's 4 s dwell: a pet (rank 5) does not outweigh a tag (rank 7)
    assert(m.now == MOOD_SAD);
    run(&m, ME_BIT(ME_SHAKE), .5f, MOOD_LIVE);         // a shake does
    assert(m.now == MOOD_SCARED);
    m = fresh();
    run(&m, ME_BIT(ME_SAY_SAD), 4.5f, MOOD_LIVE);
    run(&m, ME_BIT(ME_PET), .5f, MOOD_LIVE);          // after the dwell a pet comforts
    assert(m.now == MOOD_CALM);
}

// Refreshing: the same mood again lasts longer and is felt more, and does not count as a new mood.
static void check_refresh(void) {
    mood_machine_t m = fresh();
    mood_input_t in = event_input(ME_BIT(ME_TAP), 0.f, 0.f, MOOD_LIVE);
    mood_step(&m, &in, DT);
    assert(m.now == MOOD_CURIOUS && m.level == .5f);
    run(&m, 0, 6.f, MOOD_LIVE);
    assert(m.intensity < .3f);
    in.events = ME_BIT(ME_TAP);
    assert(!mood_step(&m, &in, DT));
    assert(m.now == MOOD_CURIOUS && m.quiet_s < .1f && fabsf(m.level - .8f) < 1e-5f && m.intensity > .75f);
}

// Two shakes in a row (a second apart): scared, then grumpy. Seven quick taps: grumpy. Playful and loved put up with twice as much.
static void check_pester(void) {
    mood_machine_t m = fresh();
    run(&m, ME_BIT(ME_SHAKE), 1.f, MOOD_LIVE);
    assert(m.now == MOOD_SCARED);
    run(&m, ME_BIT(ME_SHAKE), .1f, MOOD_LIVE);
    assert(m.now == MOOD_GRUMPY && m.cause == ME_PESTER);
    assert(m.cause_strength == .5f && m.cause_side == 0.f);  // its payload comes from the shake, not another event
    m = fresh();
    for (int tap = 0; tap < 7; tap++) run(&m, ME_BIT(ME_TAP), .25f, MOOD_LIVE);
    assert(m.now == MOOD_GRUMPY);
    m = fresh();
    for (int tap = 0; tap < 7; tap++) run(&m, ME_BIT(ME_TAP), 2.f, MOOD_LIVE);  // slowly: it forgives
    assert(m.now == MOOD_CURIOUS);
    m = fresh();
    run(&m, ME_BIT(ME_HEART), 1.f, MOOD_LIVE);
    for (int tap = 0; tap < 7; tap++) run(&m, ME_BIT(ME_TAP), .25f, MOOD_LIVE);
    assert(m.now == MOOD_LOVED);
    for (int tap = 0; tap < 8; tap++) run(&m, ME_BIT(ME_TAP), .25f, MOOD_LIVE);
    assert(m.now == MOOD_GRUMPY);
}

// A storm of taps and shakes never makes it flicker between moods: after the first seconds, a change at most every 1.5 s.
static void check_no_flicker(void) {
    unsigned seed = 99;
    for (int trial = 0; trial < 50; trial++) {
        mood_machine_t m = fresh();
        mood_input_t in = {.gate = MOOD_LIVE};
        float last = -9, t = 0;
        for (int n = 0; n < 60 * 30; n++, t += DT) {
            seed = seed * 1664525u + 1013904223u;
            in.events = (seed >> 24) < 60 ? ME_BIT((seed >> 8) % 3 ? ME_TAP : ME_SHAKE) : 0;
            memset(in.event_strength, 0, sizeof in.event_strength);
            if (in.events) in.event_strength[in.events & ME_BIT(ME_TAP) ? ME_TAP : ME_SHAKE] = .8f;
            if (mood_step(&m, &in, DT)) {
                if (t > 3.f) assert(t - last >= 1.5f - 1e-3f);
                last = t;
            }
        }
    }
}

// The blend: sums to 1, and while it goes to a mood that mood's weight only rises and every other one's only falls.
static void check_blend(void) {
    for (int to = MOOD_CURIOUS; to < MOOD_SLEEPY; to++) {
        mood_machine_t m = fresh();
        mood_input_t in = event_input(k_cause[to], 1.f, 0.f, MOOD_LIVE);
        mood_step(&m, &in, DT);
        assert(m.now == to && m.weights[MOOD_CALM] > .9f);  // begins from where it was
        float last[MOOD_COUNT];
        memcpy(last, m.weights, sizeof last);
        in.events = 0;
        for (float t = DT; t < mood_fade_s((mood_t)to) + .2f; t += DT) {
            mood_step(&m, &in, DT);
            for (int i = 0; i < MOOD_COUNT; i++) assert(i == to ? m.weights[i] >= last[i] : m.weights[i] <= last[i]);
            memcpy(last, m.weights, sizeof last);
        }
        assert(m.weights[to] == 1.f);
    }
    mood_machine_t m = fresh();  // interrupted: it starts from the mix it was in
    run(&m, ME_BIT(ME_SAY_SAD), .5f, MOOD_LIVE);
    float sad = m.weights[MOOD_SAD];
    assert(sad > 0 && sad < 1);
    run(&m, ME_BIT(ME_SHAKE), DT, MOOD_LIVE);
    assert(fabsf(m.weights[MOOD_SAD] - sad) < .02f && m.weights[MOOD_SCARED] < .05f);
}

// Across a batch, the selected cause keeps its own strength and side even beside a louder unrelated event.
static void check_mixed_event_payloads(void) {
    mood_machine_t m = fresh();
    mood_input_t in = event_input(ME_BIT(ME_TAP) | ME_BIT(ME_JOLT), 0.f, 0.f, MOOD_LIVE);
    in.event_strength[ME_TAP] = .2f; in.event_side[ME_TAP] = .7f;
    in.event_strength[ME_JOLT] = .95f; in.event_side[ME_JOLT] = -.9f;
    mood_step(&m, &in, DT);
    assert(m.now == MOOD_CURIOUS && m.cause == ME_TAP);
    assert(fabsf(m.level - .6f) < 1e-5f && m.cause_strength == .2f && m.cause_side == .7f);
}

// Sleepy: only after being CALM and drowsy (or run down and not charging) for a while; a touch wakes it; it lifts when the reason is gone.
static void check_sleepy(void) {
    mood_machine_t m = fresh();
    mood_input_t in = {.drowsy = true, .gate = MOOD_LIVE};
    for (int i = 0; i < 4 * 30; i++) mood_step(&m, &in, DT);
    assert(m.now == MOOD_CALM);
    for (int i = 0; i < 2 * 30; i++) mood_step(&m, &in, DT);
    assert(m.now == MOOD_SLEEPY && m.cause == MOOD_VIA_TIRED);
    for (int i = 0; i < 200 * 30; i++) mood_step(&m, &in, DT);
    assert(m.now == MOOD_SLEEPY);  // as long as its cause lasts
    in.drowsy = false;
    for (int i = 0; i < 6 * 30; i++) mood_step(&m, &in, DT);
    assert(m.now == MOOD_CALM);
    in = (mood_input_t){.battery_low = true, .charging = true, .gate = MOOD_LIVE};
    for (int i = 0; i < 20 * 30; i++) mood_step(&m, &in, DT);
    assert(m.now == MOOD_CALM);  // on the charger it is fine
    in.charging = false;
    for (int i = 0; i < 6 * 30; i++) mood_step(&m, &in, DT);
    assert(m.now == MOOD_SLEEPY);
    in.events = ME_BIT(ME_TOUCH);
    in.event_strength[ME_TOUCH] = .5f;
    mood_step(&m, &in, DT);
    assert(m.now == MOOD_CURIOUS);
    in.events = 0;
    m = fresh();
    in.gate = MOOD_HELD;
    for (int i = 0; i < 20 * 30; i++) mood_step(&m, &in, DT);
    assert(m.now == MOOD_CALM);  // asleep or offline it does not nod off
}

int main(void) {
    check_table();
    check_reach();
    check_decay_times();
    check_always_calm();
    check_empty_input_stays_calm();
    check_gates();
    check_priority_and_dwell();
    check_refresh();
    check_pester();
    check_no_flicker();
    check_blend();
    check_mixed_event_payloads();
    check_sleepy();
    puts("mood machine ok");
    return 0;
}
