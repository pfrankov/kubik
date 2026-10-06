// Simulator scenarios for Tess's idle moves and what it does in reaction to its surroundings (tess_mood.c).
//   sim mood            every scenario, one after another (the frame-hash stream)
//   sim mood <name>     one scenario; `sim mood list` names them
// Each scenario starts from a fresh face. The moves it went through are logged on stderr.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "../main/tess_internal.h"
#include "tess_gaze_probe.h"

typedef struct {
    const char *name;
    float seconds;
    void (*prepare)(face_t *f);   // before the first frame (after the warm-up)
    void (*act)(face_t *f, float t);  // every frame, t seconds in
} scenario_t;

static void warm_up(face_t *f, float seconds) {
    for (int i = 0; i < seconds * R_FPS; i++) face_update(f, 1.f / R_FPS);
}
static void event_tap(face_t *f, float t) { if (t == 0) face_event(f, FEV_TAP, 400, 250); }
static void event_pickup(face_t *f, float t) { if (t == 0) face_event(f, FEV_PICKUP, 1, 0); }
static void event_shake(face_t *f, float t) { if (t == 0) face_event(f, FEV_SHAKE, 0, 0); }
static void event_notify(face_t *f, float t) { if (t == 0) face_event(f, FEV_NOTIFY, 0, 0); }
static void listen(face_t *f, float t) {
    if (t == 0) face_set_mode(f, MODE_LISTENING);
    f->mic_level = .5f + .4f * sinf(t * 5);
}
static void speak(face_t *f, float t) {
    if (t == 0) face_set_mode(f, MODE_SPEAKING);
    f->spk_level = fmaxf(0, .5f + .5f * sinf(t * 6.3f));
}
static void offline(face_t *f, float t) { if (t == 0) face_set_mode(f, MODE_OFFLINE); }
static void think(face_t *f, float t) { if (t == 0) face_set_mode(f, MODE_THINKING); }
// Left alone for two minutes: the app's own drowsy face comes on at 120 s.
static void doze_off(face_t *f) {
    warm_up(f, 140);
    face_set_emotion(f, EMO_SLEEPY, -1);
}
static void flat_battery(face_t *f) { face_set_power(f, true, 9, false, false); }
static void on_charge(face_t *f) { face_set_power(f, true, 60, true, true); }
static void no_act(face_t *f, float t) {}

static const scenario_t k_scenarios[] = {
    {"idle", 24, NULL, no_act},
    {"tap", 4, NULL, event_tap},
    {"pickup", 4, NULL, event_pickup},
    {"shake", 5, NULL, event_shake},
    {"notify", 3, NULL, event_notify},
    {"listening", 4, NULL, listen},
    {"speaking", 4, NULL, speak},
    {"offline", 12, NULL, offline},
    {"sleepy", 6, doze_off, no_act},
    {"low battery", 6, flat_battery, no_act},
    {"charging", 5, on_charge, no_act},
    {"waiting", 6, NULL, think},
};
#define SCENARIOS (int)(sizeof k_scenarios / sizeof k_scenarios[0])

static void run_scenario(const scenario_t *s) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    bool idle = !strcmp(s->name, "idle");
    if (!idle) warm_up(&f, 8);  // it has been idling a while when the thing happens
    if (s->prepare) s->prepare(&f);
    int frames = (int)(s->seconds * R_FPS), last = -1;
    float w4_peak = 0;
    for (int i = 0; i < frames; i++) {
        float t = (float)i / R_FPS;
        s->act(&f, t);
        face_update(&f, 1.f / R_FPS);
        if (f.tess_mood.move != last && f.mode == MODE_IDLE) {
            static const char *const names[] = {"drift", "rest", "glance", "wobble", "4D turn", "orbit", "peek"};
            fprintf(stderr, "  %s %5.2f s: %s (%.1f s)\n", s->name, t, names[f.tess_mood.move], f.tess_mood.len);
            last = f.tess_mood.move;
        }
        w4_peak = fmaxf(w4_peak, fabsf(f.tess_mood.w_turn[2]));
        sim_render(&f);
        sim_emit_frame();
    }
    fprintf(stderr, "mood %-12s %d frames, 4D peak %.2f rad\n", s->name, frames, w4_peak);
}

void sim_mood(const char *name) {
    if (name && !strcmp(name, "list")) {
        for (int i = 0; i < SCENARIOS; i++) fprintf(stderr, "%s\n", k_scenarios[i].name);
        return;
    }
    bool found = !name;
    for (int i = 0; i < SCENARIOS; i++) {
        if (name && strcmp(name, k_scenarios[i].name)) continue;
        run_scenario(&k_scenarios[i]);
        found = true;
    }
    if (!found) {
        fprintf(stderr, "no scenario %s\n", name);
        exit(1);
    }
}

// Tess's attention (tess_gaze.c), one phase forced at a time, from a fresh face: `sim gaze <name>` (`list` names them).
typedef struct { const char *name; float seconds; void (*prepare)(face_t *f); void (*act)(face_t *f, float t); } gaze_scenario_t;

static void attend_for_good(face_t *f) { f->tess_mood.phase_left = 1000; f->tess_next_fidget = 1e9f; }
static void wander_for_good(face_t *f) {
    tess_mood_t *m = &f->tess_mood;
    m->attentive = false; m->attention = 0; m->phase_left = 1000; m->check_in = 1000; f->tess_next_fidget = 1e9f;
}
static void wander_and_check(face_t *f) { wander_for_good(f); f->tess_mood.check_in = 5; }
static void desk_tremor(face_t *f, float t) { if (t >= 2.f && t < 2.f + 1.f / R_FPS) f->jolt_dvx = .012f; }
static void peek_soon(face_t *f) { attend_for_good(f); f->tess_mood.peek_in = 2.f; }
static void tilted(face_t *f, float t) {  // the device tips over 0.7 rad in 3 s, and back
    float a = .7f * smooth01(t / 3.f) * (1.f - smooth01((t - 6.f) / 3.f)), s = sinf(a / 2);
    f->view_q[0] = cosf(a / 2); f->view_q[1] = s; f->view_q[2] = 0; f->view_q[3] = 0;
}
static void no_gaze(face_t *f, float t) {}

static const gaze_scenario_t k_gaze[] = {
    {"attentive", 10, attend_for_good, no_gaze},
    {"tilted", 10, attend_for_good, tilted},
    {"vibration", 9, wander_for_good, desk_tremor},
    {"peek", 9, peek_soon, no_gaze},
    {"wandering", 16, wander_and_check, no_gaze},
};

void sim_gaze(const char *name) {
    int count = (int)(sizeof k_gaze / sizeof k_gaze[0]);
    if (!name || !strcmp(name, "list")) {
        for (int i = 0; i < count; i++) fprintf(stderr, "%s\n", k_gaze[i].name);
        return;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(name, k_gaze[i].name)) continue;
        face_t f;
        face_init(&f);
        face_set_character(&f, CHARACTER_TESS);
        face_set_mode(&f, MODE_IDLE);
        warm_up(&f, 3);  // (it has unfolded)
        k_gaze[i].prepare(&f);
        for (int n = 0; n < (int)(k_gaze[i].seconds * R_FPS); n++) {
            float t = (float)n / R_FPS;
            k_gaze[i].act(&f, t);
            face_update(&f, 1.f / R_FPS);
            if (n % 15 == 0)
                fprintf(stderr, "  %5.1f s: %s, attention %.2f, cell %d, facing %.3f, study %.1f, peek %.2f/%.2f/%.2f\n", t, f.tess_mood.attentive ? "attentive" : "wandering",
                        f.tess_mood.attention, f.tess_mood.cell, tess_gaze_facing(&f), f.tess_mood.study, f.tess_mood.w_turn[0], f.tess_mood.w_turn[1], f.tess_mood.w_turn[2]);
            sim_render(&f);
            sim_emit_frame();
        }
        return;
    }
    fprintf(stderr, "no scenario %s\n", name);
    exit(1);
}
