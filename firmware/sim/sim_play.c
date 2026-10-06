// Simulator scenarios for what a finger does to Tess and what it does when nobody does anything (tess_touch.c, tess_play.c).
//   sim play <name>     one scenario; `sim play list` names them
// A finger script says where the finger is at each moment; it is fed to the face the way the touch task and the app do
// (50 samples a second, a tap on release, a pet after 220 px or 650 ms). The cues the face asks the speaker for are logged.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "play_script.h"
#include "../main/tess_internal.h"

typedef struct {
    const char *name;
    float seconds;
    const stroke_t *strokes;
    int count;
    void (*prepare)(face_t *f);
    void (*act)(face_t *f, float t);
} scenario_t;

static void warm_up(face_t *f, float seconds) {
    for (int i = 0; i < seconds * R_FPS; i++) face_update(f, 1.f / R_FPS);
}

static const char *const k_cue_names[TC_COUNT] = {"touch", "fling", "swing", "excite", "rub", "glance", "peek", "invite", "dodge", "startle", "content", "mischief", "love", "joy", "sad", "tinkle", "snap", "curious", "playful", "scared", "grumpy", "lonely", "settle"};

static void log_cues(face_t *f, float t, const char *name) {
    tess_cue_t cue;
    float strength, position;
    while (face_take_cue(f, &cue, &strength, &position)) fprintf(stderr, "  %s %5.2f s: cue %s (%.2f, %+.2f)\n", name, t, k_cue_names[cue], strength, position);
}

static const stroke_t k_tap[] = {{.5f, .62f, 340, 255, 340, 255}};
static const stroke_t k_rim[] = {{.5f, .62f, 410, 330, 410, 330}};
static const stroke_t k_taps[] = {{.4f, .5f, 250, 250, 250, 250}, {.9f, 1.f, 270, 260, 270, 260}, {1.4f, 1.5f, 240, 250, 240, 250},
                                  {1.9f, 2.f, 260, 260, 260, 260}, {2.4f, 2.5f, 250, 250, 250, 250}, {2.9f, 3.f, 240, 260, 240, 260}};
static const stroke_t k_swipe[] = {{.5f, .75f, 130, 255, 350, 255}};
static const stroke_t k_vertical[] = {{.5f, .75f, 240, 130, 240, 350}};
static const stroke_t k_diagonal[] = {{.5f, .75f, 150, 150, 330, 330}};
static const stroke_t k_swings[] = {{.5f, .75f, 130, 255, 350, 255}, {2.f, 2.25f, 350, 255, 130, 255}, {3.4f, 3.65f, 130, 255, 350, 255}};
static const stroke_t k_slow_drag[] = {{.5f, 1.5f, 200, 255, 300, 255}};
static const stroke_t k_hold[] = {{.5f, 3.5f, 240, 255, 240, 255}};
static const stroke_t k_pet[] = {{.5f, 4.f, 190, 250, 290, 250}};
static const stroke_t k_catch[] = {{.5f, .62f, 330, 255, 330, 255}, {1.6f, 1.72f, 300, 255, 300, 255}};
static const stroke_t k_burst[] = {{.3f, .4f, 240, 255, 240, 255}, {.6f, .7f, 240, 255, 240, 255}, {.9f, 1.f, 240, 255, 240, 255},
                                   {1.2f, 1.3f, 240, 255, 240, 255}, {1.5f, 1.6f, 240, 255, 240, 255}, {1.8f, 1.9f, 240, 255, 240, 255}};
static const stroke_t k_ripple[] = {{.4f, .5f, 170, 215, 170, 215}, {1.6f, 1.7f, 330, 300, 330, 300}};
static const stroke_t k_answer[] = {{5.f, 5.12f, 260, 255, 260, 255}};

// A vigorous rub: back and forth across the cloud.
static bool rubbing(float t, float *x, float *y) {
    *x = 240 + 60 * sinf(6.2831853f * 3.5f * t); *y = 255 + 15 * sinf(6.2831853f * 1.7f * t);
    return t >= .5f && t < 7.5f;
}

static void shake(face_t *f, float t) { if (t == .5f) face_event(f, FEV_SHAKE, 0, 0); }
// What tools/test-device.mjs does to the device: six taps 0.3 s apart, a shake, a pet.
static void burst(face_t *f, float t) {
    int frame = (int)lroundf(t * R_FPS);
    if (frame == 6 * 9) face_event(f, FEV_SHAKE, 0, 0);
    if (frame == 7 * 9) face_event(f, FEV_PET, 240, 255);
}
// A small shake of the desk, five jolts a second for three seconds.
static void buzz(face_t *f, float t) {
    int frame = (int)lroundf(t * R_FPS);
    if (frame >= R_FPS / 2 && frame < 7 * R_FPS / 2 && frame % (R_FPS / 6) == 0) f->jolt_dvx = frame / (R_FPS / 6) % 2 ? .012f : -.012f;
}
// The same touch with its wave taken away, to lay beside the real one.
static void no_wave(face_t *f, float t) { for (int k = 0; k < TESS_RIPPLES; k++) f->tess_ripple[k].age = 99; }
static void ignored(face_t *f) { f->tess_play.alone = 30; f->tess_play.invite_in = .8f; }
static void bored(face_t *f) { f->tess_play.alone = 90; f->tess_play.invites = 2; f->tess_play.invite_in = .8f; }
static void unnoticed(face_t *f) { f->tess_play.alone = 60; f->tess_play.dodge_cool = 0; }
static void offline(face_t *f) { face_set_mode(f, MODE_OFFLINE); warm_up(f, 45); }
static void no_act(face_t *f, float t) {}

static void ready_to_dodge(face_t *f) { play_ready_to_dodge(f, k_catch, 2); }

#define STROKES(a) a, (int)(sizeof a / sizeof a[0])
static const scenario_t k_scenarios[] = {
    {"tap", 3, STROKES(k_tap), NULL, no_act},
    {"tap-rim", 3, STROKES(k_rim), NULL, no_act},
    {"taps", 7, STROKES(k_taps), NULL, no_act},
    {"swipe", 6, STROKES(k_swipe), NULL, no_act},
    {"swipe-vertical", 6, STROKES(k_vertical), NULL, no_act},
    {"swipe-diagonal", 6, STROKES(k_diagonal), NULL, no_act},
    {"swings", 9, STROKES(k_swings), NULL, no_act},
    {"drag", 5, STROKES(k_slow_drag), NULL, no_act},
    {"hold", 10, STROKES(k_hold), NULL, no_act},
    {"pet", 10, STROKES(k_pet), NULL, no_act},
    {"rub", 10, NULL, 0, NULL, no_act},
    {"dodge", 5, STROKES(k_catch), ready_to_dodge, no_act},
    {"invite", 12, NULL, 0, ignored, no_act},
    {"answer", 8, STROKES(k_answer), ignored, no_act},
    {"mischief", 10, NULL, 0, bored, no_act},
    {"shake", 6, NULL, 0, NULL, shake},
    {"ripple", 4, STROKES(k_ripple), NULL, no_act},
    {"ripple-off", 4, STROKES(k_ripple), NULL, no_wave},
    {"buzz", 5, NULL, 0, NULL, buzz},
    {"burst", 6, STROKES(k_burst), NULL, burst},
    {"ignored", 10, NULL, 0, unnoticed, no_act},
    {"offline", 8, NULL, 0, offline, no_act},
    {"shimmer", 12, NULL, 0, offline, no_act},
};
#define SCENARIOS (int)(sizeof k_scenarios / sizeof k_scenarios[0])

static void run_scenario(const scenario_t *s) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    warm_up(&f, 8);
    if (s->prepare) s->prepare(&f);
    finger_t finger = {0};
    bool rub = !strcmp(s->name, "rub");
    int frames = (int)(s->seconds * R_FPS);
    for (int i = 0; i < frames; i++) {
        float t = (float)i / R_FPS;
        if (rub) {
            float x, y;
            bool down = rubbing(t, &x, &y);
            rub_touch(&finger.track, &f.rub_in, down, (int)x, (int)y);
        } else {
            play_feed(&f, &finger, s->strokes, s->count, t);
        }
        s->act(&f, t);
        face_update(&f, 1.f / R_FPS);
        log_cues(&f, t, s->name);
        sim_render(&f);
        sim_emit_frame();
    }
    fprintf(stderr, "play %-9s %d frames\n", s->name, frames);
}

void sim_play(const char *name) {
    if (name && !strcmp(name, "list")) {
        for (int i = 0; i < SCENARIOS; i++) fprintf(stderr, "%s\n", k_scenarios[i].name);
        return;
    }
    bool found = false;
    for (int i = 0; i < SCENARIOS; i++) {
        if (name && strcmp(name, k_scenarios[i].name)) continue;
        run_scenario(&k_scenarios[i]);
        found = true;
    }
    if (!found) {
        fprintf(stderr, "no scenario %s\n", name ? name : "(none)");
        exit(1);
    }
}
