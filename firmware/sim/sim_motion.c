// Simulator motion scenarios for Tess: a long tilt under gravity and uneven frame times.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"

#define GRAVITY_SECONDS 15
#define JITTER_SECONDS 12
#define ROTATE_SECONDS 12
#define SPEED_LIMIT 30.f  // fastest sane point, cloud units per second
#define POSITION_LIMIT 3.f

static void tess_start(face_t *f) {
    face_init(f);
    face_set_character(f, CHARACTER_TESS);
    face_set_mode(f, MODE_IDLE);
    for (int i = 0; i < 3 * R_FPS; i++) face_update(f, 1.f / R_FPS);
}

// The device turns about its axes: the view leans with gravity, 0.3 rad per unit as in the Tess sheet.
static void lean(face_t *f, float gx, float gy) {
    float hx = gy * .3f, hy = -gx * .3f;
    f->view_q[0] = cosf(hypotf(hx, hy));
    f->view_q[1] = sinf(hx);
    f->view_q[2] = sinf(hy);
    f->view_q[3] = 0;
}

void sim_gravity(void) {
    face_t f;
    tess_start(&f);
    face_set_mode(&f, MODE_OFFLINE);  // the loose points lie where gravity pulls them
    for (int i = 0; i < GRAVITY_SECONDS * R_FPS; i++) {
        float t = (float)i / R_FPS, turn = t < 3 ? 0 : (t - 3) * .7f;
        f.grav_x = .9f * sinf(turn);
        f.grav_y = .9f * cosf(turn);
        lean(&f, f.grav_x, f.grav_y);
        face_update(&f, 1.f / R_FPS);
        sim_render(&f);
        sim_emit_frame();
    }
    fprintf(stderr, "gravity: %d frames\n", GRAVITY_SECONDS * R_FPS);
}

// The device is turned about a tilted axis and back, the way a hand does: the view follows (view_q), gravity
// with it. Frames go out for inspection; tess_rotation_reference in the tests holds the invariants.
void sim_rotate(void) {
    face_t f;
    tess_start(&f);
    for (int i = 0; i < ROTATE_SECONDS * R_FPS; i++) {
        float t = (float)i / R_FPS, angle = .9f * sinf(t * 1.1f) * (t < ROTATE_SECONDS - 2 ? 1 : 0);
        float ax = .6f, ay = .8f * cosf(t * .7f), az = .3f * sinf(t * .4f), n = sqrtf(ax * ax + ay * ay + az * az);
        float s = sinf(angle / 2) / n;
        f.view_q[0] = cosf(angle / 2); f.view_q[1] = ax * s; f.view_q[2] = ay * s; f.view_q[3] = az * s;
        face_update(&f, 1.f / R_FPS);
        sim_render(&f);
        sim_emit_frame();
    }
    fprintf(stderr, "rotate: %d frames\n", ROTATE_SECONDS * R_FPS);
}

static float jitter_dt(unsigned *seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return (27 + (*seed >> 16) % 14) / 1000.f;  // 27..40 ms
}

// Returns the first broken invariant of the cloud, or NULL: finite, bounded, no jumps between frames.
static const char *cloud_fault(const face_t *f, float before[TESS_N][3], float dt) {
    for (int i = 0; i < TESS_N; i++)
        for (int k = 0; k < 3; k++) {
            float p = f->tess_position[i][k];
            if (!isfinite(p)) return "position not finite";
            if (fabsf(p) > POSITION_LIMIT) return "position out of bounds";
            if (fabsf(p - before[i][k]) > SPEED_LIMIT * dt) return "point jumped";
        }
    return NULL;
}

void sim_jitter(void) {
    static const face_mode_t modes[] = {MODE_IDLE, MODE_LISTENING, MODE_THINKING, MODE_SPEAKING, MODE_SLEEP, MODE_IDLE};
    face_t f;
    tess_start(&f);
    static float before[TESS_N][3];
    unsigned seed = 7;
    float t = 0, worst = 0;
    int frames = 0;
    while (t < JITTER_SECONDS) {
        float dt = jitter_dt(&seed);
        if (frames % (R_FPS * 2) == 0) face_set_mode(&f, modes[frames / (R_FPS * 2) % 6]);
        f.mic_level = f.spk_level = .5f + .4f * sinf(t * 5);
        memcpy(before, f.tess_position, sizeof before);
        face_update(&f, dt);
        const char *fault = cloud_fault(&f, before, dt);
        if (fault) {
            fprintf(stderr, "jitter: %s at frame %d, t=%.3f s\n", fault, frames, t);
            exit(1);
        }
        for (int i = 0; i < TESS_N; i++)
            for (int k = 0; k < 3; k++) worst = fmaxf(worst, fabsf(f.tess_position[i][k] - before[i][k]) / dt);
        sim_render(&f);
        sim_emit_frame();
        t += dt;
        frames++;
    }
    fprintf(stderr, "jitter: %d frames, fastest point %.1f units/s\n", frames, worst);
}
