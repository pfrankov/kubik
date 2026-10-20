#include "tess_internal.h"
#include <stdlib.h>

// An explicit action owns the sound; its actual projection supplies the energy.
// Idle camera movement and a statically deformed body must not sustain a drone.
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
    m->speed = m->tail_until = 0;
}

void tess_motion_sound_gate(face_t *f) {
    if (!tess_motion_sound_allowed(f)) tess_motion_sound_stop(f);
}

// These are finite impulses, unlike pose/w_rate, which also follow endless idle targets.
static bool reacting(const face_t *f) {
    for (int i = 0; i < TR_COUNT; i++)
        if (f->tess_reaction[i] > .02f) return true;
    return false;
}
static bool carried_motion(const tess_mood_t *m) {
    return fabsf(m->drag_speed[0]) + fabsf(m->drag_speed[1]) > .05f ||
        fabsf(m->twirl_now) + fabsf(m->shift_rate[0]) + fabsf(m->shift_rate[1]) > .1f ||
        fabsf(m->yw_rate) + fabsf(m->barrel_rate) > .05f;
}
static bool structural_motion(const tess_games_t *g) {
    if (g->self_act || g->bloom > .02f || g->game == TESS_GAME_STRETCH ||
        g->game == TESS_GAME_FLY || g->game == TESS_GAME_GATHER) return true;
    for (int i = 0; i < TESS_WAVES; i++) if (g->wave[i].amp != 0) return true;
    return false;
}
static bool touch_motion(const face_t *f) {
    const tess_play_t *p = &f->tess_play;
    bool touch = f->tess_touch_t < .65f || (p->down && fabsf(p->speed[0]) + fabsf(p->speed[1]) > 12);
    bool wiggle = (!p->down || p->petted) && f->tess_mood.wiggle > .015f;
    return touch || wiggle;
}
static float motion_threshold(face_t *f) {
    bool structural = structural_motion(&f->tess_games);
    if (touch_motion(f) || f->tess_motion_sound.input_active ||
        structural || reacting(f) || carried_motion(&f->tess_mood))
        f->tess_motion_sound.tail_until = fmaxf(f->tess_motion_sound.tail_until, f->t + (structural ? .6f : .1f));
    // Shape springs finish; other impulses already include their decay. Idle drift cannot renew either.
    if (f->t >= f->tess_motion_sound.tail_until) return INFINITY;
    return f->tess_motion_sound.sounding ? REST_PX_S : START_PX_S;
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
    if (m->speed < motion_threshold(f)) {
        if (m->sounding) tess_tone(f, TT_MOTION, 300, 0, 0);
        m->sounding = false;
        return;
    }
    float effort = fmaxf(0, m->speed - REST_PX_S);
    float energy = effort / (effort + 80.f);
    float recent = 1 - smooth01((f->tess_games.since_touch - 2.f) / 2.f);
    float level = f->tess_play.down ? .45f : .045f + .405f * recent;
    float shape = clampf(strain / (m->speed + 1), 0, 1);
    tess_tone(f, TT_MOTION, tess_tone_hz(f, .1f), energy * level, shape);
    m->sounding = true;
}
