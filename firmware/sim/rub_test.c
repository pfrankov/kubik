// Petting takes effort (rub.c, face_rub.c, tess_rub.c): a lazy swipe gets a small acknowledgement, a slow rub
// gets a little more, only sustained vigorous rubbing on the belly climbs every stage and earns the joy/heart,
// and the effort fades when the finger stops. cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rub_script.h"

#define DT (1.f / 30)

static face_t start(int character) {
    face_t f;
    face_init(&f);
    face_set_character(&f, character);
    face_set_mode(&f, MODE_IDLE);
    f.body.req = -1;
    for (int i = 0; i < 90; i++) face_update(&f, DT);
    return f;
}
static bool loved(const face_t *f) {
    return f->character == CHARACTER_TESS ? f->tess_reaction[TR_HEART] > .05f || f->tess_hold[TR_HEART] > 0 : f->emotion == EMO_LOVE;
}

typedef struct { int peak; float joy_at, stage_at[RUB_STAGES], back_to_zero, warm_peak; bool loved_early; } run_t;
static void note(run_t *r, const face_t *f, int script, float t) {
    if (f->rub.stage > r->peak) r->peak = f->rub.stage;
    if (f->rub.stage > 0 && r->stage_at[f->rub.stage] == 0) r->stage_at[f->rub.stage] = t + .001f;
    if (f->rub.joy && r->joy_at < 0) r->joy_at = t;
    if (loved(f) && r->joy_at < 0) r->loved_early = true;
    if (f->tess_mood.warm > r->warm_peak) r->warm_peak = f->tess_mood.warm;
    if (r->peak && f->rub.stage == 0 && r->back_to_zero < 0 && !rub_script_at(script, t, &(float){0}, &(float){0})) r->back_to_zero = t;
}

// Runs a script, then `after` more seconds without a finger.
static run_t run(int character, int script, float after) {
    face_t f = start(character);
    rub_finger_t finger = {0};
    run_t r = {.joy_at = -1, .back_to_zero = -1};
    float total = k_script_s[script] + after;
    for (int i = 0; i * DT < total; i++) {
        float t = i * DT;
        rub_script_feed(&f, &finger, script, t);
        face_update(&f, DT);
        note(&r, &f, script, t);
        assert(isfinite(f.tess_mood.warm) && f.rub.energy >= 0 && f.rub.energy <= 1);
    }
    return r;
}

static void one_swipe(int character) {
    face_t f = start(character);
    rub_finger_t finger = {0};
    float t = 0;
    for (int i = 0; i < 120; i++, t += DT) {
        rub_script_feed(&f, &finger, SCRIPT_SWIPE, t);
        face_update(&f, DT);
        assert(f.rub.stage <= RUB_NOTICE && !f.rub.joy && !loved(&f) && f.emotion != EMO_HAPPY && f.emotion != EMO_JOY);
    }
    assert(finger.fired_pet);  // the swipe was long enough for the app's pet event
    printf("character %d single swipe: energy %.2f, no joy, no heart\n", character, f.rub.energy);
}

static void stages_of(int character) {
    run_t slow = run(character, SCRIPT_SLOW, 0), vig = run(character, SCRIPT_VIGOROUS, 0);
    run_t stop = run(character, SCRIPT_STOP, 6), corner = run(character, SCRIPT_CORNER, 0);
    printf("character %d: slow peak %d joy %.2f | vigorous stages at %.2f %.2f %.2f %.2f, joy %.2f warm %.2f | stop peak %d\n",
           character, slow.peak, slow.joy_at, vig.stage_at[1], vig.stage_at[2], vig.stage_at[3], vig.stage_at[4], vig.joy_at, vig.warm_peak, stop.peak);
    assert(slow.peak <= RUB_NOTICE && slow.joy_at < 0 && !slow.loved_early);
    assert(vig.peak == RUB_WIGGLE);
    for (int s = 2; s < RUB_STAGES; s++) assert(vig.stage_at[s] > vig.stage_at[s - 1]);  // every stage in order
    assert(vig.stage_at[RUB_WIGGLE] > 1.f);                      // not straight away
    assert(vig.joy_at >= 2.5f && vig.joy_at <= 4.5f);            // a few seconds of it
    assert(!vig.loved_early);                                    // no heart before the reward
    assert(stop.peak >= RUB_BLUSH && stop.joy_at < 0 && !stop.loved_early);  // stopping short of the reward
    assert(stop.back_to_zero > 0 && stop.back_to_zero < 2.2f + 4.f);       // and the effort fades
    assert(corner.peak == 0 && corner.joy_at < 0);              // rubbing where it is not ticklish does nothing
    if (character == CHARACTER_TESS) assert(vig.warm_peak > .5f && slow.warm_peak < .3f);
}

// The variants of a stage: three of them, never the same twice in a row.
static void moves_vary(void) {
    rub_t r = {.stage = RUB_GIGGLE};
    int seen[3] = {0}, last = -1;
    unsigned seed = 3;
    for (int i = 0; i < 90; i++) {
        seed = seed * 1664525u + 1013904223u;
        r.move_left = 0;
        assert(rub_move_due(&r, DT));
        rub_pick_move(&r, (seed >> 8) / 16777216.f);
        assert(r.move < 3 && r.move != last);
        seen[r.move]++; last = r.move;
    }
    assert(seen[0] && seen[1] && seen[2]);
    printf("moves vary: %d %d %d picks of the three variants, never a repeat\n", seen[0], seen[1], seen[2]);
}

int main(void) {
    for (int c = 0; c < CHARACTER_COUNT; c++) { one_swipe(c); stages_of(c); }
    moves_vary();
    puts("rub ok");
    return 0;
}
