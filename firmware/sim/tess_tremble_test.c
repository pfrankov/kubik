// A small shake or a tremor of the desk makes the dots tremble, each on its own; the tesseract as a whole does not
// shake (a rigid body is only turned and carried, eased). Under a script of small jolts the pose, the carried shift and
// the centre of the cloud change smoothly frame to frame, while every dot is off its place by a different, random
// amount. (The pose still turns to look at you, eased, as it does at any tremor of the desk: only its bends are limited.)
// cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/tess_internal.h"

#define DT (1.f / 30)

static face_t rested(void) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    f.tess_next_fidget = 1e9f;
    for (int i = 0; i < 8 * 30; i++) face_update(&f, DT);
    assert(f.tess_rigid >= 1);
    return f;
}

static void centre(const face_t *f, float c[3]) {  // the middle of the targets: what the body as a whole does
    c[0] = c[1] = c[2] = 0;
    for (int i = 0; i < TESS_N; i++)
        for (int a = 0; a < 3; a++) c[a] += f->tess_target[i][a] / TESS_N;
}

typedef struct { float pose_step, pose_bend, shift_step, centre_step, common, spread, agree_sum, common_sum, speed_sum, correlation; int trembling_frames; } seen_t;

// How far every dot is off its place (position - target), and whether they move together: the mean of the dots' kicks
// (the change of their velocities from frame to frame: the slow lag of a turning body is left out) and the agreement of
// neighbouring dots' kicks (independent ones: none).
static void offsets(const face_t *f, float last_v[TESS_N][3], seen_t *s) {
    float mean[3] = {0}, sq = 0, kick_sq = 0, agree = 0, kick[TESS_N][3];
    for (int i = 0; i < TESS_N; i++)
        for (int a = 0; a < 3; a++) {
            float d = f->tess_position[i][a] - f->tess_target[i][a];
            kick[i][a] = f->tess_velocity[i][a] - last_v[i][a];
            last_v[i][a] = f->tess_velocity[i][a];
            mean[a] += kick[i][a] / TESS_N;
            sq += d * d;
            kick_sq += kick[i][a] * kick[i][a];
        }
    float spread = sqrtf(sq / (3 * TESS_N)), size = sqrtf(kick_sq / (3 * TESS_N));
    float common = sqrtf(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
    for (int i = 0; i + 1 < TESS_N; i++)
        agree += kick[i][0] * kick[i + 1][0] + kick[i][1] * kick[i + 1][1];
    if (spread > .01f) {
        s->trembling_frames++;
        s->spread = fmaxf(s->spread, spread);
        s->common_sum += common * common;
        s->agree_sum += agree / (2 * (TESS_N - 1));  // (over the whole shake, weighted by how hard it was)
        s->speed_sum += size * size;
    }
}

static seen_t shaken(float jolt, int every) {
    face_t f = rested();
    seen_t s = {0};
    float pose[TP_COUNT], last_step[TP_COUNT] = {0}, shift[2], c[3], last_v[TESS_N][3];
    memcpy(last_v, f.tess_velocity, sizeof last_v);
    memcpy(pose, f.tess_mood.pose, sizeof pose);
    memcpy(shift, f.tess_mood.shift, sizeof shift);
    centre(&f, c);
    for (int i = 0; i < 5 * 30; i++) {
        if (i < 3 * 30 && i % every == 0) f.jolt_dvx = i / every % 2 ? jolt : -jolt;  // the desk buzzes for 3 s
        face_update(&f, DT);
        for (int k = 0; k < TP_COUNT; k++) {  // a look is eased (small changes of the step); a jitter would flip it
            float step = f.tess_mood.pose[k] - pose[k];
            s.pose_step = fmaxf(s.pose_step, fabsf(step));
            s.pose_bend = fmaxf(s.pose_bend, fabsf(step - last_step[k]));
            pose[k] = f.tess_mood.pose[k];
            last_step[k] = step;
        }
        for (int k = 0; k < 2; k++) { s.shift_step = fmaxf(s.shift_step, fabsf(f.tess_mood.shift[k] - shift[k])); shift[k] = f.tess_mood.shift[k]; }
        float now[3];
        centre(&f, now);
        for (int a = 0; a < 3; a++) { s.centre_step = fmaxf(s.centre_step, fabsf(now[a] - c[a])); c[a] = now[a]; }
        offsets(&f, last_v, &s);
    }
    s.correlation = fabsf(s.agree_sum) / s.speed_sum;
    s.common = sqrtf(s.common_sum / s.speed_sum);
    return s;
}

static void small_shake_trembles_the_dots(void) {
    static const float jolt[] = {.008f, .012f, .02f, .035f};
    for (int k = 0; k < 4; k++) {
        seen_t s = shaken(jolt[k], 5);
        printf("shake %.3f g*s: dots off their places by up to %.3f units in %d frames (common motion %.0f%%, neighbours' kicks agree %.0f%%); "
               "biggest frame step: pose %.3f rad (its change %.4f), carried shift %.4f, centre %.4f units\n",
               jolt[k], s.spread, s.trembling_frames, 100 * s.common, 100 * s.correlation, s.pose_step, s.pose_bend, s.shift_step, s.centre_step);
        assert(s.trembling_frames > 30 && s.spread > .02f);  // the dots do move
        assert(s.common < .25f);                              // ... each its own way, not together
        assert(s.correlation < .25f);
        assert(s.pose_bend < .015f && s.shift_step < .01f && s.centre_step < .02f);  // the body does not shake
    }
}

static void tremble_stops_and_it_settles(void) {
    face_t f = rested();
    for (int i = 0; i < 60; i++) { f.jolt_dvx = i % 2 ? .02f : -.02f; face_update(&f, DT); }
    for (int i = 0; i < 8 * 30; i++) face_update(&f, DT);
    assert(f.tess_rigid >= 1 && !memcmp(f.tess_position, f.tess_target, sizeof f.tess_target));
}

int main(void) {
    small_shake_trembles_the_dots();
    tremble_stops_and_it_settles();
    puts("tess tremble ok");
    return 0;
}
