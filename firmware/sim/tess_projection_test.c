// Tess on screen: the integer projection stays within 1/8 px of the float formula, and a turned view moves
// the dots smoothly (no frame-to-frame jumps), whatever way the device is turned by hand. cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../main/tess_draw.c"
#include "../main/viewpoint.h"

typedef struct { float x, y, radius; int key; } ref_dot_t;  // Q3 units (1/8 px)

static void project_reference(const face_t *f, const float points[TESS_N][3], const float m[3][3], float wgain, float dust,
                              float range, float gain, ref_dot_t out[TESS_N]) {
    for (int i = 0; i < TESS_N; i++) {
        float r[3];
        tess_transform_point(m, points[i], r, false);
        float p = 4.f / fmaxf(4.7f - r[2], 1.2f);
        float d = clampf((r[2] + wgain * f->tess_wdepth[i] + range) * gain, 0, 1);
        out[i] = (ref_dot_t){(240 + r[0] * 90 * p) * 8, (255 + r[1] * 90 * p) * 8,
                             (2.f + 2.8f * d) * clampf(p, .55f, 1.5f) * dust * 8, (int)(d * 65535.f)};
    }
}

static void turn_view(face_t *f, float angle, float ax, float ay, float az) {
    float n = sqrtf(ax * ax + ay * ay + az * az), s = sinf(angle / 2) / n;
    f->view_q[0] = cosf(angle / 2);
    f->view_q[1] = ax * s; f->view_q[2] = ay * s; f->view_q[3] = az * s;
}

static face_t started(void) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    for (int i = 0; i < 90; i++) face_update(&f, 1.f / 30);
    return f;
}

// Every point of every mood at random turns, against the float projection.
static void projection_matches_float(void) {
    static const face_mode_t modes[] = {MODE_IDLE, MODE_LISTENING, MODE_THINKING, MODE_SPEAKING, MODE_SLEEP, MODE_OFFLINE};
    face_t f = started();
    srand(3);
    float worst_xy = 0, worst_radius = 0;
    for (int t = 0; t < 1200; t++) {
        if (t % 200 == 0) {
            face_set_mode(&f, modes[t / 200]);
            for (int i = 0; i < 120; i++) face_update(&f, 1.f / 30);
        }
        turn_view(&f, rand() / (float)RAND_MAX * 6.28f, rand() / (float)RAND_MAX * 2 - 1, rand() / (float)RAND_MAX * 2 - 1,
                  rand() / (float)RAND_MAX * 2 - 1 + 1e-3f);
        face_update(&f, 1.f / 30);
        float m[3][3];
        tess_view_matrix(&f, m);
        if (f.tess_fallen) for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[i][j] = i == j;
        float heart = f.tess_reaction[TR_HEART];
        float form = f.tess_fallen ? 0 : 1 - fmaxf(fmaxf(fmaxf(f.tess_mode[TM_LISTEN], heart), f.tess_reaction[TR_SCATTER]), f.tess_drift);
        float wgain = .9f * form, dust = 1 - .35f * f.tess_drift, range = 1.7f + .6f * form - heart * .7f, gain = 1 / (2 * range);
        dot_t dots[TESS_N];
        ref_dot_t ref[TESS_N];
        project_points(&f, f.tess_position, m, 4.7f, wgain, dust, range, gain, dots);
        project_reference(&f, f.tess_position, m, wgain, dust, range, gain, ref);
        for (int i = 0; i < TESS_N; i++) {
            float dx = fabsf(dots[i].x - ref[i].x), dy = fabsf(dots[i].y - ref[i].y), dr = fabsf(dots[i].radius - ref[i].radius);
            if (dx > worst_xy) worst_xy = dx;
            if (dy > worst_xy) worst_xy = dy;
            if (dr > worst_radius) worst_radius = dr;
        }
    }
    printf("projection vs float: worst position %.3f, radius %.3f (1/8 px)\n", worst_xy, worst_radius);
    assert(worst_xy <= 1.f);  // 1/8 px
    assert(worst_radius <= 1.f);
}

// A steady turn of the view moves every dot by a steady amount: the change of a dot's step from one frame to
// the next stays small (a jitter shows as steps that alternate).
#define STEP_CHANGE_LIMIT 4.f  // Q3 units (half a px) at 30 fps
// The cloud's own life (a fidget kick at 10.8 s of this script) moves a dot by up to 85 units (10.6 px) more than the
// frame before, as it does at the reviewed HEAD; a projection, spring or matching change must not make it worse.
#define LIFE_STEP_CHANGE_LIMIT 96.f
static float turning_view_step_change(bool moving) {
    face_t f = started();
    static dot_t dots[3][TESS_N];
    float worst = 0;
    for (int i = 0; i < 12 * 30; i++) {
        float t = i / 30.f;
        turn_view(&f, .9f * sinf(t * 1.1f), .6f, .8f * cosf(t * .7f), .3f * sinf(t * .4f) + 1e-3f);
        if (moving) face_update(&f, 1.f / 30);
        float m[3][3];
        tess_view_matrix(&f, m);
        dot_t *now = dots[i % 3], *before = dots[(i + 2) % 3], *earlier = dots[(i + 1) % 3];
        project_points(&f, f.tess_position, m, 4.7f, .9f, 1.f, 2.3f, 1 / 4.6f, now);
        if (i < 2) continue;
        for (int k = 0; k < TESS_N; k++) {
            float ddx = (now[k].x - before[k].x) - (before[k].x - earlier[k].x);
            float ddy = (now[k].y - before[k].y) - (before[k].y - earlier[k].y);
            if (fabsf(ddx) > worst) worst = fabsf(ddx);
            if (fabsf(ddy) > worst) worst = fabsf(ddy);
        }
    }
    return worst;
}

// ---- The device turned by hand: gyro scripts through the real viewpoint filter (as input.c feeds it: 20 ms
// samples, one frame every 1/30 s), the cloud held still so that only the view moves.
typedef struct { const char *name; float rate[3]; float seconds; } turn_t;
#define SHAKE_AXIS .577f
static const turn_t k_turns[][8] = {  // rad/s about the body axes, each for `seconds`
    {{"spin about x", {2, 0, 0}, 6.5f}},
    {{"spin about y", {0, 2, 0}, 6.5f}},
    {{"spin about z", {0, 0, 2}, 6.5f}},
    {{"tilt +-90 about x", {1.5f, 0, 0}, 1.05f}, {"", {-1.5f, 0, 0}, 2.1f}, {"", {1.5f, 0, 0}, 1.05f}},
    {{"tilt +-90 about y", {0, 1.5f, 0}, 1.05f}, {"", {0, -1.5f, 0}, 2.1f}, {"", {0, 1.5f, 0}, 1.05f}},
    {{"flip 180 and back", {3.1416f, 0, 0}, 1.f}, {"", {-3.1416f, 0, 0}, 1.f}, {"", {0, 3.1416f, 0}, 1.f}, {"", {0, -3.1416f, 0}, 1.f}},
    {{"tumble", {1.2f, 1.7f, .8f}, 9.f}},
    {{"fast shake", {8 * SHAKE_AXIS, 8 * SHAKE_AXIS, 8 * SHAKE_AXIS}, .15f}, {"", {-8 * SHAKE_AXIS, -8 * SHAKE_AXIS, -8 * SHAKE_AXIS}, .15f}},
};
#define SCRIPTS (int)(sizeof k_turns / sizeof k_turns[0])
#define TURN_DT .02f
// Rotation of the view between two frames, from the traces of the matrices (radians).
static float matrix_step(const float a[3][3], const float b[3][3]) {
    float trace = 0;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) trace += a[i][j] * b[i][j];
    return acosf(clampf((trace - 1) / 2, -1, 1));
}
static float quaternion_step(const float a[4], const float b[4]) {
    float dot = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2 * acosf(clampf(dot, 0, 1));
}
static const float VIEW_STEP_PER_TURN = .9f;  // the view never turns faster than this fraction of the device (gain .4)
static const float DOT_JUMP_PER_TURN = 3000.f;  // 1/8 px of a dot's move per radian of the device's turn between frames
// The script's gyro rate `t` seconds in (the script repeats).
static const float *rate_at(const turn_t *script, float t) {
    float total = 0;
    for (const turn_t *turn = script; turn->seconds > 0; turn++) total += turn->seconds;
    t = fmodf(t, total);
    while (t >= script->seconds) t -= script++->seconds;
    return script->rate;
}
typedef struct { dot_t dots[TESS_N]; ref_dot_t ref[TESS_N]; float m[3][3], pose[4]; } turn_frame_t;
static void device_turns_are_continuous(void) {
    static turn_frame_t frames[2];
    face_t f = started();
    float worst_view = 0, worst_dot = 0, worst_reference = 0;
    for (int s = 0; s < SCRIPTS; s++) {
        viewpoint_t v;
        viewpoint_init(&v);
        float worst_script = 0, next_frame = 0;
        int shown = 0;
        for (int step = 0; step * TURN_DT < 26; step++) {
            viewpoint_step(&v, rate_at(k_turns[s], step * TURN_DT), false, TURN_DT);
            if (step * TURN_DT < next_frame) continue;
            next_frame += 1.f / 30;
            turn_frame_t *now = &frames[shown & 1], *last = &frames[~shown & 1];
            memcpy(f.view_q, v.p, sizeof f.view_q);
            memcpy(now->pose, v.p, sizeof now->pose);
            tess_view_matrix(&f, now->m);
            project_points(&f, f.tess_position, now->m, 4.7f, .9f, 1.f, 2.3f, 1 / 4.6f, now->dots);
            project_reference(&f, f.tess_position, now->m, .9f, 1.f, 2.3f, 1 / 4.6f, now->ref);
            if (!shown++) continue;
            float jump = 0, reference = 0;
            for (int i = 0; i < TESS_N; i++) {
                jump = fmaxf(jump, hypotf(now->dots[i].x - last->dots[i].x, now->dots[i].y - last->dots[i].y));
                reference = fmaxf(reference, hypotf(now->ref[i].x - last->ref[i].x, now->ref[i].y - last->ref[i].y));
            }
            float device_step = quaternion_step(last->pose, now->pose), view_step = matrix_step(last->m, now->m);
            float over_view = view_step - VIEW_STEP_PER_TURN * device_step, over_dot = jump - DOT_JUMP_PER_TURN * device_step;
            worst_view = fmaxf(worst_view, over_view);
            worst_dot = fmaxf(worst_dot, over_dot);
            worst_reference = fmaxf(worst_reference, fabsf(jump - reference));
            worst_script = fmaxf(worst_script, jump);
            if (over_view > .01f || over_dot > 24)
                printf("  %s: %.2f s: device %.3f rad, view %.3f rad, dot jump %.0f (1/8 px)\n", k_turns[s][0].name, step * TURN_DT, device_step, view_step, jump);
        }
        printf("device turn '%s': biggest dot jump in a frame %.0f (1/8 px)\n", k_turns[s][0].name, worst_script);
    }
    printf("device turns: view over its share %.4f rad, dot over its share %.1f (1/8 px), integer vs float jump %.2f\n", worst_view, worst_dot, worst_reference);
    assert(worst_view <= .01f);
    assert(worst_dot <= 24.f);
    assert(worst_reference <= 3.f);  // the integer projection is within 1/8 px on each end of a step
}

int main(void) {
    device_turns_are_continuous();
    float still = turning_view_step_change(false), moving = turning_view_step_change(true);
    printf("turning view: worst change of a dot's step %.1f (cloud held), %.1f (cloud moving), 1/8 px\n", still, moving);
    assert(still <= STEP_CHANGE_LIMIT);
    assert(moving <= LIFE_STEP_CHANGE_LIMIT);
    projection_matches_float();
    puts("tess projection ok");
    return 0;
}
