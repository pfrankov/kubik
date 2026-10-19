#include "tess_games_internal.h"
#include "face_math.h"
#include <math.h>
#include <string.h>

// The core of Tess's play: who may play, how touch reaches her structure, what she has discovered and how
// big that has made her. Nothing here draws: every behaviour moves her own dots (tess_play_shape.c).
//   A tap on her plucks the nearest corner (ordinary tap reactions still happen around it).
//   A finger that lands beside her pulls and slings her (tess_stretch.c); in flight a tap catches her.
//   Swipes add up into spin, and spin opens her (tess_play_spin).
//   A burst (a hard landing, or six quick taps) hangs for a sweep (tess_gather.c).
//   Alone, she plays by herself. The agent, voice, menus, offline and dark stop all of it at once.
#define PI 3.14159265f
#define ON_HER_PX 150.f      // a tap this near her centre plucks her
#define SELF_FIRST 18.f

static bool visible(const face_t *f) {
    return !f->dark && !f->menu.open && f->menu.k < .45f && f->mode != MODE_SETUP;
}
bool tess_games_eligible(const face_t *f) {
    return f->tess_games.ready && f->tess_games.available && face_character(f) == CHARACTER_TESS &&
        visible(f) && f->mode == MODE_IDLE && !f->live_active && !f->card_n && !f->card_hold &&
        !f->tess_fallen && f->emotion != EMO_SLEEPY;
}
bool tess_games_active(const face_t *f) {
    return (f->tess_games.game != TESS_GAME_NONE || f->tess_games.sky_hold_owned) && tess_games_eligible(f);
}
bool tess_games_owns_tap(const face_t *f) { return tess_games_active(f) || f->tess_games.finger_owned; }

static unsigned discoveries(uint8_t progress) {
    unsigned count = 0;
    for (; progress; progress >>= 1) count += progress & 1u;
    return count;
}
static float grown_form(uint8_t progress) {
    unsigned count = discoveries(progress);
    return count == 6 ? 3 : count >= 3 ? 2 : count ? 1 : 0;
}
void tess_games_restore(face_t *f, uint8_t progress) {
    tess_games_t *g = &f->tess_games;
    uint32_t round = g->round + 1;
    if (KUBIK_CHARACTER == CHARACTER_TESS && g->sky_audio) tess_sky_stop(f);
    tess_sky_release(g);
    memset(g, 0, sizeof *g);
    g->round = round ? round : 1;
    g->ready = g->available = true;
    g->progress = progress & TESS_PROGRESS_MASK;
    g->form = grown_form(g->progress);
    g->self_wait = SELF_FIRST;
    g->answer_in = -1;
    g->since_pluck = g->since_touch = 99;
    tess_gather_clear(g);
}
// Everything that moves her stops; springs carry her dots home from wherever they are.
void tess_games_leave(tess_games_t *g) {
    g->game = TESS_GAME_NONE;
    g->phase = TESS_GAME_REST;
    g->streak = 0;
    g->sky_invite_t = 0;
    tess_sky_release(g);
    g->pull_k = 0;  // (her body keeps where it is and how it moves: a round never teleports her)
    tess_self_end(g);
}
static void stop_all(tess_games_t *g) {
    tess_games_leave(g);
    for (int w = 0; w < TESS_WAVES; w++) g->wave[w].amp = 0;
    g->squash_k = 0;
    g->answer_in = -1;  // (her temper stays: she remembers what happened)
}
void tess_games_set_available(face_t *f, bool available) {
    f->tess_games.available = available;
    if (!available) {
        if (KUBIK_CHARACTER == CHARACTER_TESS) tess_sky_stop(f);
        stop_all(&f->tess_games);
    }
}
// A first discovery: she lights up gold for a moment and says so. Repeats add nothing.
void tess_games_earn(face_t *f, unsigned bit) {
    tess_games_t *g = &f->tess_games;
    if (g->progress & (1u << bit)) return;
    g->progress |= (uint8_t)(1u << bit);
    g->pulse = 1;
    tess_react(f, TR_JOY, 1.2f);
    tess_cue(f, TC_JOY, .6f, 0);
}

void tess_games_begin(face_t *f, tess_game_t game) {
    tess_games_t *g = &f->tess_games;
    tess_games_leave(g);
    if (!++g->round) g->round = 1;
    g->game = (uint8_t)game;
    g->phase = TESS_GAME_SHOW;
    g->age = g->clock = g->idle = 0;
    memset(f->tess_play.after_what, 0xff, sizeof f->tess_play.after_what);
    f->tess_play.cue_n = f->tess_play.invites = 0;
    f->tess_play.waiting = f->tess_play.dodge_t = 0;
    f->tess_play.down = false;
    f->tess_taps = 0;
    f->rub = (rub_t){0};
    f->rub_love = false;
    f->tess_rigid = 0;
}
void tess_games_celebrate(face_t *f, bool glad) {
    tess_games_t *g = &f->tess_games;
    g->phase = TESS_GAME_CELEBRATE;
    g->clock = 0;
    if (!glad) return;
    g->pulse = 1;
    tess_react(f, TR_JOY, 1.6f);
    tess_cue(f, TC_JOY, .7f, 0);
}

// Developer previews: each behaviour on demand, through the same paths a finger uses.
bool tess_games_start(face_t *f, tess_preview_t play, unsigned tier) {
    tess_games_t *g = &f->tess_games;
    if (!tess_games_eligible(f) || !f->tess_points_ready) return false;
    g->tier = (uint8_t)tier;
    switch (play) {
    case TESS_PLAY_PLUCK: tess_play_pluck(f, 0, false); return true;
    case TESS_PLAY_BLOOM: f->tess_mood.drag_speed[0] = 14.f; return true;
    case TESS_PLAY_STRETCH:
        if (!tess_stretch_try(f, 420, 255)) return false;
        g->pull[0] = 455; g->pull[1] = 215;
        return true;
    case TESS_PLAY_FLY:
        tess_games_begin(f, TESS_GAME_FLY);
        g->phase = TESS_GAME_WAIT;
        g->fly_v[0] = 9.f; g->fly_v[1] = -5.f;
        return true;
    case TESS_PLAY_GATHER:
        tess_scatter_begin(f);
        f->tess_hold[TR_SCATTER] = fminf(f->tess_hold[TR_SCATTER], TESS_BURST_S - TESS_HANG_S);
        tess_gather_track(f, 0);
        return tess_gather_try_start(f);
    case TESS_PLAY_SELF: g->self_wait = 0; return true;
    case TESS_PLAY_SKY: return KUBIK_CHARACTER == CHARACTER_TESS && tess_sky_start(f);
    default: return false;
    }
}

// ---------------------------------------------------------------- touch
static bool panel_position(float x, float y) { return x >= 0 && x < 480 && y >= 0 && y < 480; }
static void pluck_tap(face_t *f, float x, float y) {
    float cx, cy;
    tess_games_centre(f, &cx, &cy);
    if (hypotf(x - cx, y - cy) > ON_HER_PX || !f->tess_points_ready) return;
    tess_temper_pluck(f, tess_play_nearest_vertex(f, x, y, NULL));
}
// A round's own events: true when it consumed them.
static bool round_event(face_t *f, face_event_t event, float x, float y) {
    tess_games_t *g = &f->tess_games;
    if (g->game == TESS_GAME_SKY && event == FEV_PICKUP) return true;
    if (g->game == TESS_GAME_SKY && event == FEV_SHAKE) return tess_games_end_sky(f);
    if (event == FEV_TAP) {
        if (g->game == TESS_GAME_FLY && !g->pressed) tess_fly_tap(f, x, y);  // a tap too quick for a frame
        g->pressed = g->finger_owned = false;
        return true;
    }
    if (event == FEV_PET && g->game != TESS_GAME_FLY) return true;  // a held pull or sweep is not petting
    if (event == FEV_WAKE || event == FEV_VOLUME || event == FEV_SHAKE) return false;
    tess_games_leave(g);
    return false;
}
// The agent's and the voice's events stop all play at once; a wake, volume or shake does not.
static bool agent_event(face_event_t event) {
    return event == FEV_NOTIFY || event == FEV_NOT_HEARD || event == FEV_FAIL || event == FEV_TALK_START;
}
// With no round going: a tap may answer her invitation or pluck her; petting soothes her.
static bool idle_event(face_t *f, face_event_t event, float x, float y) {
    if (KUBIK_CHARACTER == CHARACTER_TESS && event == FEV_TAP && tess_sky_tap(f, x, y)) return true;
    if (event == FEV_TAP) pluck_tap(f, x, y);
    if (event == FEV_PET) tess_temper_feel(f, 0, -.25f, -.25f, .15f);
    return false;
}
bool tess_games_event(face_t *f, face_event_t event, float x, float y) {
    tess_games_t *g = &f->tess_games;
    bool allowed = tess_games_eligible(f);
    if (!allowed || agent_event(event)) stop_all(g);
    if (event == FEV_TAP && !panel_position(x, y)) return true;  // malformed coordinates never reach reactions
    if (allowed && g->game) return round_event(f, event, x, y);
    if (event == FEV_TAP && g->finger_owned) {
        g->finger_owned = g->pressed = false;  // the release of a finger whose round has ended
        return true;
    }
    return allowed && idle_event(f, event, x, y);
}

static void finger_lands(face_t *f, const rub_input_t *in) {
    tess_games_t *g = &f->tess_games;
    g->finger_age = g->finger_moved = 0;
    g->finger_x = in->x; g->finger_y = in->y;
    g->touch_at[0] = in->x; g->touch_at[1] = in->y;  // she remembers where
    g->since_touch = 0;
    tess_self_end(g);  // a finger takes her attention from her own play
    g->pressed = false;
    if (g->game == TESS_GAME_FLY) { tess_fly_tap(f, in->x, in->y); g->pressed = true; }  // its tap must not press again
    g->finger_owned = g->game != TESS_GAME_NONE || tess_gather_try_start(f) || tess_stretch_try(f, in->x, in->y);
    g->sweep[0] = in->x; g->sweep[1] = in->y;
    if (g->game == TESS_GAME_GATHER) tess_gather_sweep(f, in->x, in->y, in->x, in->y);
}
static void finger_moves(face_t *f, float dt, const rub_input_t *in) {
    tess_games_t *g = &f->tess_games;
    g->finger_age += dt;
    g->finger_moved = fmaxf(g->finger_moved, hypotf(in->x - g->finger_x, in->y - g->finger_y));
    if (g->game == TESS_GAME_GATHER) {
        tess_gather_sweep(f, g->sweep[0], g->sweep[1], in->x, in->y);
        g->sweep[0] = in->x; g->sweep[1] = in->y;
    } else if (g->game == TESS_GAME_STRETCH) tess_stretch_move(f, in->x, in->y);
}
static void pointer(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    const rub_input_t *in = &f->rub_in;
    if (g->sky_hold_owned) { g->finger_down = in->down; g->lifted = 0; return; }
    if (in->down && !g->finger_down) finger_lands(f, in);
    else if (in->down) finger_moves(f, dt, in);
    else if (g->finger_down && g->game == TESS_GAME_STRETCH) tess_stretch_release(f);
    g->lifted = in->down ? 0 : g->lifted + dt;
    if (g->lifted > .5f && !g->game) g->finger_owned = g->pressed = false;  // a tap that never came
    g->finger_down = in->down;
}

// ---------------------------------------------------------------- the clock
static bool quiet(const face_t *f) {
    return !f->rub_in.down && f->rub.stage == 0 && f->tess_hold[TR_HEART] <= 0 &&
        f->tess_hold[TR_SCATTER] <= 0 && f->tess_agitation < .2f && f->tess_mode[TM_SLEEP] < .3f;
}
static void animate(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    bool conversation = f->live_active || f->mode == MODE_THINKING || f->mode == MODE_SPEAKING;
    float wanted = conversation ? 3 : grown_form(g->progress), step = dt * 1.2f;
    g->form += clampf(wanted - g->form, -step, step);
    g->fold = 0;
    tess_games_centre(f, &g->hit[0], &g->hit[1]);
    g->hit_valid = true;
    g->squash_k *= expf(-dt * 5.f);
    if (g->squash_k < .002f) g->squash_k = 0;
    for (int k = 0; k < 2; k++) {
        g->offset[k] = g->fly[k];  // her body is wherever its physics put it (tess_body_update)
        if (fabsf(g->offset[k]) < .0005f && fabsf(g->fly_v[k]) < .001f) g->offset[k] = g->fly[k] = 0;
    }
}
static void advance(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    tess_gather_track(f, dt);
    tess_temper_update(f, dt);
    tess_temper_jolt(f, hypotf(f->jolt_dvx, f->jolt_dvy));
    pointer(f, dt);
    tess_play_waves(f, dt);
    if (g->game == TESS_GAME_STRETCH) tess_stretch_update(f, dt);
    else if (g->game == TESS_GAME_GATHER) tess_gather_update(f, dt);
    else if (KUBIK_CHARACTER == CHARACTER_TESS && g->game == TESS_GAME_SKY) tess_sky_update(f, dt);
    else if (!g->game && quiet(f)) tess_self_update(f, dt);
}
void tess_games_update(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    if (!g->ready || face_character(f) != CHARACTER_TESS || !isfinite(dt) || dt <= 0) return;
    dt = fminf(dt, .1f);
    bool allowed = tess_games_eligible(f);
    if (!allowed) stop_all(g);
    if (!f->rub_in.down && !allowed) g->finger_down = false;
    if (KUBIK_CHARACTER == CHARACTER_TESS) tess_sky_tick(f, dt);
    if (!visible(f)) return;  // preemption above still runs when rendering is paused
    g->pulse = fmaxf(0, g->pulse - dt * 3.f);
    if (allowed) advance(f, dt);
    else tess_gather_track(f, dt);
    tess_body_update(f, dt);
    tess_play_spin(f, dt);
    animate(f, dt);
}
