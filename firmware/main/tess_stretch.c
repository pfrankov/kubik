#include "tess_games_internal.h"
#include "face_math.h"
#include <math.h>

// Her body as a physical thing. A finger that lands on the glass beside her (not on her) takes hold of her
// nearest corner: she stretches after it like taffy and creaks higher the tauter she gets (scared, she shies
// away instead; fond, all of her follows). Let go close and she twangs back. Pull far and she flies the other
// way. Tilt the device and she rolls downhill; level it and she rolls home. Wherever she goes, the rim of the
// glass stops her: she flattens on the side that hit, thuds, and her dots rattle; a hard enough hit after a
// throw shatters her into the burst (tess_gather.c). A tap on her in flight catches her.
// Left alone she plays softly by herself, as her temper suggests (tess_temper.c).
#define PI 3.14159265f
#define BESIDE_PX 120.f      // a finger this far from her centre is beside her, not on her
#define BODY_PX 90.f         // her own reach: a pull counts from here
#define TWANG_UNITS .3f
#define SLING_UNITS .9f
#define RIM_UNITS 1.45f      // her centre bounces at this distance from the panel's
#define BOUNCE .6f           // restitution
#define SHATTER_SPEED 12.f   // units/s into the rim that bursts her: only a pull to the very edge
#define CATCH_PX 95.f
#define ROLL_ACCEL 14.f      // units/s² per unit of tilt: about half a tilt presses her to the rim
#define HOME_SPRING 3.5f     // 1/s²: her pull back to the middle when nothing holds her elsewhere
#define SELF_LO 16.f
#define SELF_HI 30.f

void tess_games_centre(const face_t *f, float *x, float *y) {
    *x = GAME_CX + f->tess_games.offset[0] * TESS_PX_PER_UNIT;
    *y = GAME_CY + f->tess_games.offset[1] * TESS_PX_PER_UNIT;
}
static float stretched(const face_t *f) {  // units beyond her own reach
    float cx, cy;
    tess_games_centre(f, &cx, &cy);
    return (hypotf(f->tess_games.pull[0] - cx, f->tess_games.pull[1] - cy) - BODY_PX) / TESS_PX_PER_UNIT;
}

bool tess_stretch_try(face_t *f, float x, float y) {
    tess_games_t *g = &f->tess_games;
    float cx, cy;
    tess_games_centre(f, &cx, &cy);
    if (g->game || !tess_games_eligible(f) || hypotf(x - cx, y - cy) < BESIDE_PX) return false;
    int vertex = tess_play_nearest_vertex(f, x, y, NULL);
    tess_games_begin(f, TESS_GAME_STRETCH);
    g->phase = TESS_GAME_WAIT;
    g->grab = (uint8_t)vertex;
    g->pull[0] = x; g->pull[1] = y;
    g->pull_k = 0;
    tess_tone(f, tess_temper_scared(f) ? TT_WOBBLE : TT_CREAK, tess_tone_hz(f, .3f), .18f, .2f);
    return true;
}
void tess_stretch_move(face_t *f, float x, float y) {
    tess_games_t *g = &f->tess_games;
    if (g->game != TESS_GAME_STRETCH) return;
    g->pull[0] = x; g->pull[1] = y;
}
// The pull has a short onset and release sound; holding it does not sustain a tone.
void tess_stretch_update(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    g->pull_k = fminf(1, g->pull_k + dt * 6.f);
    g->age += dt;
    if (!g->finger_down && g->age > 1.5f) tess_stretch_release(f);  // (a preview has no finger to lift)
}
static void sling(face_t *f, float pulled) {
    tess_games_t *g = &f->tess_games;
    float cx, cy;
    tess_games_centre(f, &cx, &cy);
    float dx = g->pull[0] - cx, dy = g->pull[1] - cy, d = fmaxf(1, hypotf(dx, dy)), speed = fminf(15.f, 3.f + 6.f * pulled);
    tess_games_begin(f, TESS_GAME_FLY);
    g->phase = TESS_GAME_WAIT;
    g->fly_v[0] = -dx / d * speed; g->fly_v[1] = -dy / d * speed;
    tess_tone(f, TT_WHOOSH, 200.f + 30.f * speed, fminf(1, speed / 15.f) * .6f, .5f);
}
// The finger let go. A touch that never pulled is an ordinary tap, which follows as usual.
void tess_stretch_release(face_t *f) {
    tess_games_t *g = &f->tess_games;
    if (g->game != TESS_GAME_STRETCH) return;
    float pulled = stretched(f), taut = clampf(pulled / 1.8f, 0, 1);
    bool touch = g->finger_moved < 12.f && g->finger_age < .35f;
    g->pull_k = 0;  // she snaps back on her springs: the twang
    if (touch) { tess_games_leave(g); g->finger_owned = g->pressed = false; return; }
    if (pulled > TWANG_UNITS) {
        tess_games_earn(f, BIT_TWANG);
        tess_tone(f, TT_TWANG, tess_tone_hz(f, .25f + .65f * taut), .35f + .5f * taut, taut);
        tess_temper_feel(f, .15f, 0, 0, pulled < SLING_UNITS ? .05f : 0);
    }
    if (pulled > SLING_UNITS) { sling(f, pulled); return; }
    tess_games_leave(g);
}

// ---------------------------------------------------------------- her body, wherever it goes
static void rattle(face_t *f, float into, float nx) {
    int grains = into > 6.f ? 2 : into > 2.5f ? 1 : 0;
    for (int i = 0; i < grains; i++)
        tess_tone_later(f, TT_GRAIN, tess_tone_hz(f, .4f + .3f * frand(f)), .25f + .03f * into, .5f + .5f * nx, .03f + .045f * i + .02f * frand(f));
}
static void hit_rim(face_t *f, float nx, float ny, float into) {
    tess_games_t *g = &f->tess_games;
    if (into < .8f) return;
    g->squash[0] = nx; g->squash[1] = ny;
    g->squash_k = fminf(1, into / 9.f);
    g->pulse = fmaxf(g->pulse, fminf(1, into / 10.f));
    float size_hz = 200.f * (1.f + .15f * (3.f - g->form));  // a bigger Tess thuds lower; the speaker has no bass below 200 Hz
    tess_tone(f, TT_THUD, size_hz, fminf(1, .2f + into / 11.f), fminf(1, into / 12.f));
    rattle(f, into, nx);
    tess_temper_feel(f, .1f, 0, into / 30.f, 0);
    if (into > 6.f) tess_feel_event(f, ME_JOLT, fminf(1, into / 12.f), clampf(nx, -1, 1));
    if (g->game != TESS_GAME_FLY) return;
    tess_games_earn(f, BIT_BOUNCE);
    if (into < SHATTER_SPEED) return;
    for (int i = 0; i < 3; i++)  // she bursts: a few of her dots, falling
        tess_tone_later(f, TT_GRAIN, tess_tone_hz(f, .5f + .3f * frand(f)), .6f - .1f * i, .5f, .05f * i + .03f * frand(f));
    tess_temper_feel(f, .2f, 0, .5f, 0);
    tess_games_leave(g);
    tess_scatter_begin(f);  // her dots hang for a sweep, or to be poured home
}
static void rim(face_t *f, float *p, float *v) {
    float r = hypotf(p[0], p[1]);
    if (r <= RIM_UNITS) return;
    float nx = p[0] / r, ny = p[1] / r, into = v[0] * nx + v[1] * ny;
    p[0] = nx * RIM_UNITS; p[1] = ny * RIM_UNITS;
    if (into <= 0) return;
    v[0] -= (1 + BOUNCE) * into * nx; v[1] -= (1 + BOUNCE) * into * ny;
    hit_rim(f, nx, ny, into);
}
// Thrown, rolled by the tilt, or simply sitting: one body on one set of forces.
// Her pull home: none while she flies fast, soft (she sloshes) at play, firm and calm when the agent comes first.
static void home_spring(const face_t *f, bool flying, float speed, float *spring, float *damping) {
    if (!tess_games_eligible(f)) { *spring = 40.f; *damping = 12.f; return; }
    *spring = flying && speed > 2.f ? 0 : HOME_SPRING;
    *damping = *spring ? 1.2f : 0;
}
void tess_body_update(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    float *p = g->fly, *v = g->fly_v, spring, damping;
    bool flying = g->game == TESS_GAME_FLY, free = tess_games_eligible(f) && (flying || g->game == TESS_GAME_NONE);
    home_spring(f, flying, hypotf(v[0], v[1]), &spring, &damping);
    float ax = free ? g->tilt[0] * ROLL_ACCEL : 0, ay = free ? g->tilt[1] * ROLL_ACCEL : 0;
    ax += -p[0] * spring - v[0] * damping;
    ay += -p[1] * spring - v[1] * damping;
    float drag = expf(-dt * .8f);
    v[0] = (v[0] + ax * dt) * drag; v[1] = (v[1] + ay * dt) * drag;
    p[0] += v[0] * dt; p[1] += v[1] * dt;
    if (free) rim(f, p, v);
    if (g->game != TESS_GAME_FLY) return;
    g->age += dt;
    if (hypotf(p[0], p[1]) < .03f && hypotf(v[0], v[1]) < .1f) tess_games_leave(g);
}
bool tess_fly_tap(face_t *f, float x, float y) {
    tess_games_t *g = &f->tess_games;
    float cx, cy;
    tess_games_centre(f, &cx, &cy);
    if (g->game != TESS_GAME_FLY || hypotf(x - cx, y - cy) > CATCH_PX) return false;
    g->fly_v[0] = g->fly_v[1] = 0;  // caught: she wriggles in the hand, then goes home
    g->pulse = 1;
    tess_kick(f, KICK_WIGGLE, 1.f);
    tess_tone(f, TT_CHIRP, tess_tone_hz(f, .75f), .5f, .9f);
    tess_temper_feel(f, .2f, 0, 0, .1f);
    return true;
}

// ---------------------------------------------------------------- alone
static void begin_self(face_t *f) {
    tess_games_t *g = &f->tess_games;
    float angle = frange(f, 0, 2 * PI);
    int vertex = (int)(frand(f) * 16) & 15;
    tess_self_t act = tess_temper_self_act(f);
    g->self_spinning = act == SELF_SPIN;
    // Still excited from play, she soon asks for more; otherwise she plays now and then.
    g->self_wait = g->excite > .5f ? frange(f, 3.f, 6.f) : frange(f, SELF_LO, SELF_HI);
    if (act == SELF_PLUCK) tess_play_pluck(f, vertex, true);
    else if (act == SELF_SPIN) {
        tess_tone(f, TT_WHOOSH, tess_tone_hz(f, .2f), .08f, .15f);
        f->tess_mood.drag_speed[0] += cosf(angle) * 10.f;
        f->tess_mood.drag_speed[1] += sinf(angle) * 4.f;
    }
}
void tess_self_end(tess_games_t *g) {
    g->self_spinning = false;
}
void tess_self_update(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    g->self_wait -= dt;
    if (g->self_wait <= 0) begin_self(f);
}
