// Finger scripts for the play scenarios and tests: strokes say where the finger is at each moment, and play_feed hands
// them to the face the way the touch task and the app do (50 samples a second, a tap on release, a pet after 220 px or 650 ms).
#pragma once
#include <math.h>
#include "../main/face.h"
#include "../main/tess_internal.h"

typedef struct { float t0, t1, x0, y0, x1, y1; } stroke_t;  // the finger goes from (x0, y0) to (x1, y1) between t0 and t1

typedef struct { rub_track_t track; float next_sample, path, down_s, last_x, last_y; bool down, fired_pet; } finger_t;

// Where the finger is at t: the stroke it is in, easing along it (false: lifted).
static bool play_finger_at(const stroke_t *strokes, int count, float t, float *x, float *y) {
    for (int i = 0; i < count; i++) {
        const stroke_t *k = &strokes[i];
        if (t < k->t0 || t >= k->t1) continue;
        float u = k->t1 > k->t0 ? (t - k->t0) / (k->t1 - k->t0) : 0;
        *x = k->x0 + (k->x1 - k->x0) * u;
        *y = k->y0 + (k->y1 - k->y0) * u;
        return true;
    }
    return false;
}

// The touch task's samples up to t, and the app's events from them.
static void play_feed(face_t *f, finger_t *finger, const stroke_t *strokes, int count, float t) {
    for (; finger->next_sample <= t; finger->next_sample += .02f) {
        float x, y;
        bool down = play_finger_at(strokes, count, finger->next_sample, &x, &y);
        rub_touch(&finger->track, &f->rub_in, down, (int)x, (int)y);
        if (down && !finger->down) { finger->path = finger->down_s = 0; finger->fired_pet = false; finger->last_x = x; finger->last_y = y; }
        if (down) {
            finger->path += hypotf(x - finger->last_x, y - finger->last_y);
            finger->last_x = x; finger->last_y = y;
            finger->down_s += .02f;
            if (!finger->fired_pet && (finger->path > 220 || finger->down_s > .65f)) { finger->fired_pet = true; face_event(f, FEV_PET, x, y); }
        } else if (finger->down && !finger->fired_pet && finger->path < 40 && finger->down_s < .45f) {
            face_event(f, FEV_TAP, finger->last_x, finger->last_y);
        }
        finger->down = down;
    }
}

// Which dice make it dodge is up to the face: try a few draws of its random numbers, each with this finger, until one
// lands it a dodge (a neglected face that gets touched: 1 in 5); the face then goes on from that draw.
static inline void play_ready_to_dodge(face_t *f, const stroke_t *strokes, int count) {
    f->tess_play.alone = 40;
    f->tess_play.dodge_cool = 0;
    for (int draws = 0; draws < 200; draws++) {
        face_t probe = *f;
        finger_t finger = {0};
        for (int k = 0; k < draws; k++) frand(&probe);
        for (int i = 0; i < 30 && probe.tess_play.dodge_t <= 0; i++) {
            play_feed(&probe, &finger, strokes, count, i / 30.f);
            face_update(&probe, 1.f / 30);
        }
        if (probe.tess_play.dodge_t > 0) {
            for (int k = 0; k < draws; k++) frand(f);
            return;
        }
    }
}
