// Simulator scenarios for Tess's moods (tess_feel.c): what each mood looks like, and how one turns into the next.
//   sim feel <mood>         a clip: 1 s of calm, the thing that puts Tess in that mood (calm: nothing), then the mood as it plays out
//   sim feel                every mood's clip, one after another (the frame-hash stream)
//   sim feel sheet          one frame per mood, all in the same pose and the same second (tile them into a sheet)
//   sim feel transitions    a row of frames per mood change, from the moment of the trigger (tile them 6 to a row)
// The moods the machine goes through are logged on stderr. tools/tess-mood-captures.py turns these into gifs and sheets.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "../main/tess_internal.h"

#define SETTLE_S 3        // s of calm before anything happens, so that Tess has unfolded
#define CLIP_S 9          // s of a mood clip after its trigger
#define SLEEPY_CLIP_S 14  // (it takes five drowsy seconds before it nods off)
#define SHEET_SETTLE_FRAMES 9  // the sheet: frames for the mood to come up (the pose hardly moves meanwhile)
#define ROW_FRAMES 6      // frames of a transition row, and the s after the trigger each is taken at
static const float k_row_at[ROW_FRAMES] = {0.f, .3f, .6f, 1.f, 1.6f, 2.6f};

typedef struct {
    mood_t mood;
    mood_event_t event;   // what puts Tess in it; ME_COUNT: the drowsy face of the app, ME_COUNT + 1: nothing
    float strength;
} cause_t;
#define NOTHING (ME_COUNT + 1)

static const cause_t k_causes[] = {
    {MOOD_CURIOUS, ME_TAP, .6f}, {MOOD_PLAYFUL, ME_GLAD, .8f}, {MOOD_LOVED, ME_HEART, 1.f}, {MOOD_SCARED, ME_SHAKE, 1.f},
    {MOOD_GRUMPY, ME_SAY_ANGRY, .6f}, {MOOD_SAD, ME_SAY_SAD, .6f}, {MOOD_SLEEPY, ME_COUNT, 1.f},
};
#define CAUSES (int)(sizeof k_causes / sizeof k_causes[0])
static const cause_t k_calm = {MOOD_CALM, NOTHING, 0};  // (the clip of the neutral Tess: what it does with nothing happening)

static void fresh_face(face_t *f) {
    face_init(f);
    face_set_character(f, CHARACTER_TESS);
    face_set_mode(f, MODE_IDLE);
    sim_mood_setup(f);
    for (int i = 0; i < SETTLE_S * R_FPS; i++) face_update(f, 1.f / R_FPS);
}

static void cause(face_t *f, const cause_t *c) {
    if (c->event == NOTHING) return;
    if (c->event == ME_COUNT) face_set_emotion(f, EMO_SLEEPY, -1);
    else tess_feel_event(f, c->event, c->strength, 0);
}

static void frame(face_t *f) {
    face_update(f, 1.f / R_FPS);
    sim_render(f);
    sim_emit_frame();
}

static void log_mood(const face_t *f, float t, int *last) {
    if (f->mood.now == *last) return;
    fprintf(stderr, "  %5.2f s: %s (by event %d, level %.2f)\n", t, mood_name((mood_t)f->mood.now), f->mood.cause, f->mood.level);
    *last = f->mood.now;
}

static void clip(const cause_t *c) {
    face_t f;
    fresh_face(&f);
    int last = -1, frames = (int)((c->mood == MOOD_SLEEPY ? SLEEPY_CLIP_S : CLIP_S) * R_FPS);
    fprintf(stderr, "feel %s\n", mood_name(c->mood));
    for (int i = 0; i < R_FPS + frames; i++) {
        if (i == R_FPS) cause(&f, c);
        frame(&f);
        log_mood(&f, (float)i / R_FPS, &last);
    }
}

// The moods side by side: one Tess, set into each mood in turn and looked at a moment later (so the pose is the same and
// only what the mood does to the colours, the camera and the dots differs).
static void sheet(void) {
    face_t base;
    fresh_face(&base);
    for (int m = 0; m < MOOD_COUNT; m++) {
        face_t f = base;
        if (m == MOOD_SLEEPY) face_set_emotion(&f, EMO_SLEEPY, -1);
        face_force_mood(&f, (mood_t)m, 1.f);
        for (int i = 0; i < SHEET_SETTLE_FRAMES; i++) face_update(&f, 1.f / R_FPS);
        frame(&f);
        fprintf(stderr, "%s\n", mood_name((mood_t)m));
    }
}

// From each mood to the next, by the cause that leads there: how the body, the camera and the colours move together.
static void transitions(void) {
    for (int k = 0; k < CAUSES; k++) {
        face_t f;
        fresh_face(&f);
        bool nods_off = k_causes[k].mood == MOOD_SLEEPY;  // (it comes only from calm)
        face_force_mood(&f, k && !nods_off ? k_causes[k - 1].mood : MOOD_CALM, 1.f);
        f.mood.pinned = false;
        for (int i = 0; i < 2 * R_FPS; i++) face_update(&f, 1.f / R_FPS);
        cause(&f, &k_causes[k]);
        if (nods_off) {  // it nods off after five drowsy seconds: look at two-second steps
            for (int n = 0; n < ROW_FRAMES; n++) {
                for (int i = 0; i < R_FPS * (n ? 2 : 0); i++) face_update(&f, 1.f / R_FPS);
                frame(&f);
            }
        } else {
            float now = 0;
            for (int n = 0; n < ROW_FRAMES; n++) {
                for (; now < k_row_at[n] - 1e-3f; now += 1.f / R_FPS) face_update(&f, 1.f / R_FPS);
                frame(&f);
            }
        }
        fprintf(stderr, "%s -> %s (now %s)\n", k && !nods_off ? mood_name(k_causes[k - 1].mood) : "calm", mood_name(k_causes[k].mood), mood_name((mood_t)f.mood.now));
    }
}

void sim_feel(const char *name) {
    if (name && !strcmp(name, "sheet")) return sheet();
    if (name && !strcmp(name, "transitions")) return transitions();
    bool found = !name || !strcmp(name, "calm");
    if (found) clip(&k_calm);
    for (int i = 0; i < CAUSES; i++) {
        if (name && strcmp(name, mood_name(k_causes[i].mood))) continue;
        clip(&k_causes[i]);
        found = true;
    }
    if (!found) {
        fprintf(stderr, "no mood %s\n", name);
        exit(1);
    }
}
