#include "tess_games_internal.h"
#include "face_math.h"
#include <math.h>

// Her temper: what has been happening shapes how she answers next. Four feelings build from play and fade on
// their own (excitement, annoyance, fear, fondness); with the mood machine's colours and words (tess_feel.c)
// they decide whether a tap rings, giggles, answers back, reflects or makes her flinch away, whether a pull
// finds her eager or shy, and what she plays by herself. Her voice comes from the same place: the mood sets
// where her resonance rests, her size its register. And she feels the device's tilt (relative to how it usually stands).
#define PI 3.14159265f
#define REST_TAU 20.f        // s for "how it usually stands" to follow a new resting pose
#define TILT_DEAD .06f

// ---------------------------------------------------------------- voice
static mood_t ruling_mood(const face_t *f) {
    mood_t best = MOOD_CALM;
    for (int m = MOOD_CURIOUS; m < MOOD_COUNT; m++) if (f->feel.mix[m] > f->feel.mix[best] && f->feel.mix[m] > .3f) best = (mood_t)m;
    return best;
}
// Her body's resonance: one continuous register, never a scale, so no run of sounds can turn into a tune. Her mood
// sets where it rests (the roots of her words' keys: D, B when cross, sad or drowsy, F# when scared), her size how
// high (a small Tess rings a fifth up), and `height` lifts it smoothly up to a fifth: tension, speed, a dot's place.
float tess_tone_hz(const face_t *f, float height) {
    const tess_games_t *g = &f->tess_games;
    mood_t mood = ruling_mood(f);
    float base = 293.66f;
    if (mood == MOOD_GRUMPY || mood == MOOD_SAD || mood == MOOD_SLEEPY) base = 246.94f;
    else if (mood == MOOD_SCARED) base = 369.99f;
    if (g->ready && g->form < 1.5f) base *= 1.5f;  // a small Tess has a small voice
    return base * (1.f + .5f * clampf(height, 0, 1));
}
bool tess_temper_scared(const face_t *f) { return f->tess_games.fear > .5f || ruling_mood(f) == MOOD_SCARED; }

// ---------------------------------------------------------------- temper
void tess_temper_feel(face_t *f, float excite, float annoy, float fear, float fond) {
    tess_games_t *g = &f->tess_games;
    g->excite = clampf(g->excite + excite, 0, 1);
    g->annoy = clampf(g->annoy + annoy, 0, 1);
    g->fear = clampf(g->fear + fear, 0, 1);
    g->fond = clampf(g->fond + fond, 0, 1);
}
static void tilt_update(tess_games_t *g, const face_t *f, float dt) {
    float gx = isfinite(f->grav_x) ? f->grav_x : 0, gy = isfinite(f->grav_y) ? f->grav_y : 0, k = fminf(1, dt / REST_TAU);
    if (!g->tilt_known) { g->tilt_rest[0] = gx; g->tilt_rest[1] = gy; g->tilt_known = true; }  // however it stands now is "level"
    g->tilt_rest[0] += (gx - g->tilt_rest[0]) * k;
    g->tilt_rest[1] += (gy - g->tilt_rest[1]) * k;
    for (int a = 0; a < 2; a++) {
        float t = (a ? gy : gx) - g->tilt_rest[a];
        g->tilt[a] = fabsf(t) < TILT_DEAD ? 0 : t - copysignf(TILT_DEAD, t);
    }
}
void tess_temper_update(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    g->excite = fmaxf(0, g->excite - dt * .08f);
    g->annoy = fmaxf(0, g->annoy - dt * .07f);
    g->fear = fmaxf(0, g->fear - dt * .1f);
    g->fond = fmaxf(0, g->fond - dt * .02f);
    g->since_pluck = fminf(99, g->since_pluck + dt);
    g->since_touch = fminf(999, g->since_touch + dt);
    tilt_update(g, f, dt);
    if (g->answer_in >= 0) {  // her own reply to a pluck
        g->answer_in -= dt;
        if (g->answer_in < 0) {
            tess_tone(f, TT_CHIRP, tess_tone_hz(f, .7f), .4f, .8f);
            tess_play_pluck_as(f, g->answer_corner, false, .8f, 1.f);
        }
    }
}

// ---------------------------------------------------------------- a tap on her
// The same tap rarely gets the same answer: annoyed, she recoils from it and turns away; the same corner again
// at once makes her giggle (a quick, high ring); excited, she may answer from the opposite corner after a
// moment; once in a while the wave reflects from the far corner and rings back.
void tess_temper_pluck(face_t *f, int vertex) {
    tess_games_t *g = &f->tess_games;
    bool again = g->since_pluck < 1.2f, same = again && g->last_corner == vertex;
    tess_temper_feel(f, .12f, again ? .16f + (same ? .14f : 0) : 0, 0, 0);
    g->since_pluck = 0;
    g->last_corner = (uint8_t)vertex;
    float roll = frand(f);
    if (g->annoy > .7f) {
        tess_play_pluck_as(f, vertex, false, -.8f, 1.f);  // she pulls the corner in and away
        tess_kick(f, KICK_FLINCH, 1.f);
        tess_tone(f, TT_CHIRP, tess_tone_hz(f, .1f), .45f, .1f);
        tess_feel_event(f, ME_PESTER, .8f, 0);
        g->annoy -= .3f;
        return;
    }
    if (same && g->excite > .25f) {
        tess_play_pluck_as(f, vertex, false, .8f, .55f);  // a giggle: quick and high
        tess_react(f, TR_JOY, .6f);
        return;
    }
    tess_play_pluck_as(f, vertex, false, .85f + .3f * frand(f), .85f + .3f * frand(f));
    if (g->excite > .45f && roll < .3f && g->answer_in < 0) { g->answer_corner = (uint8_t)(vertex ^ 15); g->answer_in = .55f + .3f * frand(f); }
    else if (roll > .9f && g->answer_in < 0) { g->answer_corner = (uint8_t)(vertex ^ 15); g->answer_in = .5f; }  // it reflects back
}

// ---------------------------------------------------------------- alone
// Her own play is a quiet pluck or spin. Corner pulls belong only to the user's finger.
// Cross or scared, she keeps to herself.
uint8_t tess_temper_self_act(face_t *f) {
    tess_games_t *g = &f->tess_games;
    if (g->annoy > .4f || g->fear > .5f) return SELF_NONE;
    mood_t mood = ruling_mood(f);
    if (g->excite > .3f || mood == MOOD_PLAYFUL) return frand(f) < .5f ? SELF_SPIN : SELF_PLUCK;
    static const uint8_t k_alone[] = {SELF_PLUCK, SELF_SPIN};
    unsigned count = g->since_touch > 60.f ? 1 : 2;
    return k_alone[g->self_next++ % count];
}

// ---------------------------------------------------------------- knocks
// A knock on the device rattles her: a few corners jiggle and her dots click against each other, as many and as
// loud as the knock was hard. A hard knock startles her a little.
void tess_temper_jolt(face_t *f, float jolt) {
    tess_games_t *g = &f->tess_games;
    if (jolt < .015f || g->since_pluck < .25f) return;
    float hard = clampf(jolt * 8.f, 0, 1);
    int corners = 1 + (int)(hard * 2.5f);
    for (int i = 0; i < corners; i++) tess_play_pluck_as(f, (int)(frand(f) * 16) & 15, true, .35f + .5f * hard, .6f + .3f * frand(f));
    for (int i = 0; i < (hard > .5f ? 2 : 1); i++)
        tess_tone_later(f, TT_GRAIN, tess_tone_hz(f, .4f + .4f * frand(f)), .3f + .4f * hard, frand(f), .07f * i + .03f * frand(f));
    tess_temper_feel(f, .05f, 0, hard * .25f, 0);
    g->since_pluck = 0;
}
