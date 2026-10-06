// Tess's attention (tess_gaze.c): in an attentive phase its face cell points at the viewer, also when the device is
// tilted; the phases alternate without a period; a peek is a quarter turn in a 4D plane that hands the face to
// another cell; a tremor of the desk makes it notice you and peer, a shake or noise does not; and everything it
// adds moves smoothly. cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include "../main/tess_internal.h"
#include "tess_gaze_probe.h"

#define DT (1.f / 30)
#define FACING_MIN .975f  // cos of the angle between its face and the viewer: 13 degrees, saccades and the spring's lag included

static face_t fresh(void) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    f.tess_next_fidget = 1e9f;  // no kicks: the attention alone
    face_mood_off(&f, true);    // (and no mood: a sad Tess looks down, a curious one leans in)
    return f;
}

static void tilt(face_t *f, float angle, const float axis[3]) {  // the device turned by `angle` about the axis
    float s = sinf(angle / 2) / sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    f->view_q[0] = cosf(angle / 2);
    for (int k = 0; k < 3; k++) f->view_q[k + 1] = axis[k] * s;
}

// Turned away on purpose: a peek through the 4th dimension, or a twirl (it lets go of facing you for the spin, and takes a
// few seconds to come round again).
static bool peeking(const tess_mood_t *m) {
    if (m->free_t > 0) return true;
    for (int k = 0; k < 3; k++)
        if (fabsf(m->w_turn[k] - m->w_goal[k]) > .05f) return true;
    return false;
}

typedef struct {
    float in_phase, since_peek, worst_cos, attentive_s, wandering_s, glance_step, turn_step;
    int settled, glances_back, peeks, cell_changes, phases;
    float last_glance[2], last_turn[3], since, shortest, longest;
    bool was_attentive;
    uint8_t cell;
} watch_t;

static watch_t watch_new(const face_t *f) {
    watch_t w = {.since_peek = 9, .worst_cos = 1, .shortest = 1e9f, .was_attentive = f->tess_mood.attentive, .cell = f->tess_mood.cell};
    for (int k = 0; k < 2; k++) w.last_glance[k] = f->tess_mood.glance[k];
    for (int k = 0; k < 3; k++) w.last_turn[k] = f->tess_mood.w_turn[k];
    return w;
}

static void track_motion(const tess_mood_t *m, watch_t *w) {
    for (int k = 0; k < 2; k++) { w->glance_step = fmaxf(w->glance_step, fabsf(m->glance[k] - w->last_glance[k])); w->last_glance[k] = m->glance[k]; }
    for (int k = 0; k < 3; k++) { w->turn_step = fmaxf(w->turn_step, fabsf(m->w_turn[k] - w->last_turn[k])); w->last_turn[k] = m->w_turn[k]; }
    if (m->cell != w->cell) { w->cell_changes++; w->cell = m->cell; }
}

static void track_phases(const tess_mood_t *m, watch_t *w) {
    w->since += DT;
    if (m->attentive == w->was_attentive) return;
    w->phases++;
    w->shortest = fminf(w->shortest, w->since); w->longest = fmaxf(w->longest, w->since);
    w->since = 0;
    w->was_attentive = m->attentive;
}

// One frame: what the face does, from the outside.
static void step(face_t *f, watch_t *w) {
    float goals = f->tess_mood.w_goal[0] + f->tess_mood.w_goal[1] + f->tess_mood.w_goal[2];
    face_update(f, DT);
    const tess_mood_t *m = &f->tess_mood;
    w->in_phase = m->attentive ? w->in_phase + DT : 0;
    if (m->attentive) w->attentive_s += DT; else w->wandering_s += DT;
    if (m->check > 0 && !m->attentive) w->glances_back++;
    if (m->w_goal[0] + m->w_goal[1] + m->w_goal[2] != goals) w->peeks++;
    track_motion(m, w);
    track_phases(m, w);
    w->since_peek = peeking(m) ? 0 : w->since_peek + DT;
    if (w->in_phase > 3.f && w->since_peek > 4.f && m->attention > .97f && m->eager < .05f) {  // settled and looking at you
        w->worst_cos = fminf(w->worst_cos, tess_gaze_facing(f));
        w->settled++;
    }
}

// `seconds` of idle with the device tilted (over the first two seconds, not at once) as given.
static watch_t run(face_t *f, float seconds, float angle, const float axis[3]) {
    watch_t w = watch_new(f);
    for (int i = 0; i < (int)(seconds * 30); i++) {
        if (angle != 0) tilt(f, angle * fminf(1, i / 60.f), axis);
        step(f, &w);
    }
    return w;
}

static void faces_the_viewer(void) {
    static const float x[3] = {1, 0, 0}, y[3] = {0, 1, 0}, z[3] = {0, 0, 1}, xyz[3] = {1, 1, .5f};
    static const struct { float angle; const float *axis; } tilts[] = {{0, x}, {.7f, x}, {-.7f, x}, {.6f, y}, {-.6f, y}, {.9f, xyz}, {.5f, z}, {-.8f, xyz}};
    for (int t = 0; t < 8; t++) {
        face_t f = fresh();
        watch_t w = run(&f, 120, tilts[t].angle, tilts[t].axis);
        printf("tilt %+.1f rad: %d settled attentive frames, the face %.1f degrees off the viewer at worst (attentive %.0f s, wandering %.0f s)\n",
               tilts[t].angle, w.settled, acosf(w.worst_cos) * 57.3f, w.attentive_s, w.wandering_s);
        assert(w.settled > 300 && w.worst_cos >= FACING_MIN);
        assert(w.attentive_s > 40 && w.wandering_s > 30);  // both kinds of phase, for a good part of the time each
    }
}

// Phases have no period; glances back happen while it wanders; peeks turn a quarter and hand the face on; nothing jumps.
static void rhythm_and_peeks(void) {
    face_t f = fresh();
    watch_t w = watch_new(&f);
    for (int i = 0; i < 400 * 30; i++) {
        step(&f, &w);
        assert(f.tess_xw == 0 && f.tess_zw == 0);  // waiting's own turns are not touched
    }
    printf("400 s: %d phases (%.1f .. %.1f s), %d frames of glancing back, %d peeks, the face changed cell %d times, biggest frame step: saccade %.3f rad, peek %.3f rad\n",
           w.phases, w.shortest, w.longest, w.glances_back, w.peeks, w.cell_changes, w.glance_step, w.turn_step);
    assert(w.phases >= 25 && w.longest - w.shortest > 5.f);  // no fixed period
    assert(w.glances_back > 60 && w.peeks >= 3 && w.cell_changes >= 1);
    assert(w.glance_step < .02f && w.turn_step < .07f);
}

// A tremor of the desk: it turns to you and peers, but only for a tremor: not noise, not a shake.
static void tremor_makes_it_look(void) {
    static const float jolt[] = {.001f, .3f, .012f};
    for (int c = 0; c < 3; c++) {
        face_t f = fresh();
        watch_t w = watch_new(&f);
        while (f.tess_mood.attentive || f.tess_mood.attention > .01f) step(&f, &w);  // until it is wandering and looking away
        f.tess_mood.phase_left = f.tess_mood.check_in = 100;
        f.jolt_dvx = jolt[c];
        float peak_study = 0, peak_roll = 0;
        bool noticed = false;
        for (int i = 0; i < 6 * 30; i++) {
            step(&f, &w);
            noticed |= f.tess_mood.attentive;
            peak_study = fmaxf(peak_study, f.tess_mood.study);
            peak_roll = fmaxf(peak_roll, fabsf(f.tess_mood.pose[TP_ROLL]));
        }
        printf("tremor %.3f g*s: noticed %d, peering %.1f s, head rock %.3f rad\n", jolt[c], noticed, peak_study, peak_roll);
        if (c == 2) assert(noticed && peak_study > 3.f && peak_roll > .04f);
        else assert(peak_study == 0 && (c == 1 || !noticed));  // (a shake makes it eager, as being picked up does, but it does not peer)
    }
}

int main(void) {
    faces_the_viewer();
    rhythm_and_peeks();
    tremor_makes_it_look();
    puts("tess gaze ok");
    return 0;
}
