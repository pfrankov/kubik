#include "tess_internal.h"
#include <stdlib.h>

// Listen to the geometry that was actually drawn, including the view tilt,
// rigid turns (whose solver velocity is zero), springs, growth and inertia.
// Integer differences reuse the projection; the composer receives at most 20 updates/s.
#define SAMPLE_S .05f
#define START_PX_S 8.f
#define REST_PX_S 4.f

static bool audible_context(const face_t *f) {
    return face_character(f) == CHARACTER_TESS && !f->dark && !f->dimmed &&
        !f->menu.open && f->menu.k < .01f && !f->card_n && !f->live_active;
}
bool tess_motion_sound_allowed(const face_t *f) {
    return audible_context(f) && !f->tess_fallen && f->mode != MODE_OFFLINE &&
        f->mode != MODE_SETUP && f->mode != MODE_SLEEP &&
        f->mode != MODE_LISTENING && f->mode != MODE_SPEAKING && f->mode != MODE_THINKING &&
        f->emotion != EMO_SLEEPY && f->tess_games.game != TESS_GAME_SKY;
}

void tess_motion_sound_stop(face_t *f) {
    tess_motion_sound_t *m = &f->tess_motion_sound;
    if (m->sounding) tess_tone(f, TT_MOTION, 300, 0, 0);
    m->ready = m->sounding = false;
    m->speed = 0;
}

void tess_motion_sound_gate(face_t *f) {
    if (!tess_motion_sound_allowed(f)) tess_motion_sound_stop(f);
}

// The idle body's endless rigid drift is silent. A touch, a deforming body or a
// carried impulse owns a resonance, including the spring/momentum tail.
static bool reacting(const face_t *f) {
    for (int i = 0; i < TR_COUNT; i++)
        if (f->tess_reaction[i] > .02f) return true;
    return false;
}
static bool carried_motion(const tess_mood_t *m) {
    float pose_rate = 0;
    for (int k = 0; k < TP_COUNT; k++) pose_rate += fabsf(m->rate[k]);
    return fabsf(m->drag_speed[0]) + fabsf(m->drag_speed[1]) > .05f ||
        fabsf(m->twirl_now) + fabsf(m->shift_rate[0]) + fabsf(m->shift_rate[1]) > .1f ||
        fabsf(m->yw_rate) + fabsf(m->barrel_rate) > .05f || m->wiggle > .004f || pose_rate > .08f ||
        fabsf(m->w_rate[0]) + fabsf(m->w_rate[1]) + fabsf(m->w_rate[2]) > .05f;
}
static bool expressive_motion(const face_t *f) {
    return f->tess_play.down || f->tess_touch_t < 1.5f ||
        f->tess_rigid < .98f || f->tess_games.shaping || reacting(f) ||
        carried_motion(&f->tess_mood);
}

static float motion_threshold(const face_t *f, bool turning) {
    if (!turning && !expressive_motion(f)) return INFINITY;
    return f->tess_motion_sound.sounding ? REST_PX_S : START_PX_S;
}

static bool view_moved(tess_motion_sound_t *m, const float q[4]) {
    float same = 0, opposite = 0;
    for (int k = 0; k < 4; k++) {
        same += fabsf(q[k] - m->view[k]); opposite += fabsf(q[k] + m->view[k]);
        m->view[k] = q[k];
    }
    return fminf(same, opposite) > .002f; // q and -q are the same orientation; ignore sensor noise
}

static float measure(tess_motion_sound_t *m, const tess_projected_t dots[TESS_N], float *extent) {
    int travel = 0, reach = 0;
    for (int i = 0; i < TESS_N; i++) {
        int x = dots[i].x < -16384 ? -16384 : dots[i].x > 16384 ? 16384 : dots[i].x;
        int y = dots[i].y < -16384 ? -16384 : dots[i].y > 16384 ? 16384 : dots[i].y;
        travel += abs(x - m->xy[i][0]) + abs(y - m->xy[i][1]);
        reach += abs(x - 1920) + abs(y - 2040);
        m->xy[i][0] = (int16_t)x; m->xy[i][1] = (int16_t)y;
    }
    *extent = reach * (1.f / (8 * TESS_N));
    return travel * (1.f / (8 * TESS_N));
}

void tess_motion_sound_observe(face_t *f, const tess_projected_t dots[TESS_N]) {
    tess_motion_sound_t *m = &f->tess_motion_sound;
    if (!tess_motion_sound_allowed(f)) { tess_motion_sound_stop(f); return; }
    float dt = f->t - m->at;
    if (m->ready && dt < SAMPLE_S) return; // repeated rendering cannot replay a sound
    float extent, travel = measure(m, dots, &extent);
    bool turning = view_moved(m, f->view_q);
    bool continuous = m->ready && dt <= .25f;
    m->ready = true; m->at = f->t;
    float strain = continuous ? fabsf(extent - m->extent) / dt : 0;
    m->extent = extent;
    if (!continuous) {
        if (m->sounding) tess_tone(f, TT_MOTION, 300, 0, 0);
        m->speed = 0; m->sounding = false;
        return; // no jump on first draw/resume
    }
    float speed = fminf(travel / dt, 900);
    m->speed += (speed - m->speed) * fminf(1, dt * 12);
    if (m->speed < motion_threshold(f, turning)) {
        if (m->sounding) tess_tone(f, TT_MOTION, 300, 0, 0);
        m->sounding = false;
        return;
    }
    float energy = m->speed / (m->speed + 65.f);
    float recent = 1 - smooth01((f->tess_games.since_touch - 4.f) / 3.f);
    float level = f->tess_play.down ? .8f : .14f + .66f * recent;
    float shape = clampf(strain / (m->speed + 1), 0, 1);
    float hz = clampf(310.f - extent * .4f + energy * 110.f + shape * 80.f, 170, 480);
    tess_tone(f, TT_MOTION, hz, energy * level, shape);
    m->sounding = true;
}
