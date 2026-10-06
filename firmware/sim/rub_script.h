// Finger gestures for the rubbing scenarios and tests: a script says where the finger is at each moment, and
// rub_script_feed hands it to the face the way the touch task does (50 samples a second, one stroke tracker).
#pragma once
#include <math.h>
#include "../main/face.h"

typedef enum { SCRIPT_SWIPE, SCRIPT_SLOW, SCRIPT_VIGOROUS, SCRIPT_STOP, SCRIPT_CORNER, SCRIPT_COUNT } rub_script_t;
static const char *const k_script_names[SCRIPT_COUNT] = {"swipe", "slow", "vigorous", "stop", "corner"};
static const float k_script_s[SCRIPT_COUNT] = {4, 8, 9, 9, 6};  // how long each runs

typedef struct { rub_track_t track; float next_sample, path, last_x, last_y; bool fired_pet; } rub_finger_t;

// Where the finger is at t seconds (false: lifted).
static bool rub_script_at(int script, float t, float *x, float *y) {
    const float turn = 2 * 3.14159265f;
    switch (script) {
    case SCRIPT_SWIPE:  // one long stroke across the belly
        *x = 140 + 200 * (t / .4f); *y = 290 + 150 * (t / .4f);
        return t < .4f;
    case SCRIPT_SLOW:  // a lazy rub, 2 strokes every 3 s
        *x = 240 + 45 * sinf(turn * .7f * t); *y = 370;
        return t < 8;
    case SCRIPT_CORNER:  // as vigorous, but nowhere near the belly
        *x = 50 + 60 * sinf(turn * 3.5f * t); *y = 60;
        return t < 6;
    default:  // vigorous, all over the belly; `stop` lifts the finger after 2.2 s
        *x = 240 + 60 * sinf(turn * 3.5f * t); *y = 370 + 15 * sinf(turn * 1.7f * t);
        return script == SCRIPT_VIGOROUS ? t < 9 : t < 2.2f;
    }
}

// Feeds the samples up to t into the face (as EV_TOUCH samples and, past 220 px, the app's FEV_PET).
static void rub_script_feed(face_t *f, rub_finger_t *finger, int script, float t) {
    for (; finger->next_sample <= t; finger->next_sample += .02f) {
        float x, y;
        bool down = rub_script_at(script, finger->next_sample, &x, &y);
        rub_touch(&finger->track, &f->rub_in, down, (int)x, (int)y);
        if (!down) { finger->path = 0; finger->last_x = finger->last_y = 0; continue; }
        if (finger->last_x) finger->path += hypotf(x - finger->last_x, y - finger->last_y);
        finger->last_x = x; finger->last_y = y;
        if (finger->path > 220 && !finger->fired_pet) { finger->fired_pet = true; face_event(f, FEV_PET, x, y); }
    }
}
