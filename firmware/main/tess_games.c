#include "tess.h"
#include "face_math.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265f
#define TAP_PAUSE 1.4f
#define ROUND_LIMIT 20.f
#define TRICK_INTERVAL 40.f

static bool visible(const face_t *f) {
    return !f->dark && !f->menu.open && f->menu.k < .45f && f->mode != MODE_SETUP;
}
static bool eligible(const face_t *f) {
    return f->tess_games.ready && f->tess_games.available && face_character(f) == CHARACTER_TESS &&
        visible(f) && f->mode == MODE_IDLE && !f->live_active && !f->card_n && !f->card_hold &&
        !f->tess_fallen && f->emotion != EMO_SLEEPY;
}
bool tess_games_active(const face_t *f) { return f->tess_games.game != TESS_GAME_NONE && eligible(f); }

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
    memset(g, 0, sizeof *g);
    g->round = round ? round : 1;
    g->ready = g->available = true;
    g->progress = progress & TESS_PROGRESS_MASK;
    g->form = grown_form(g->progress);
    g->last_tap = 99;
    g->trick_wait = TRICK_INTERVAL;
}
static void leave(tess_games_t *g) {
    g->game = TESS_GAME_NONE;
    g->phase = TESS_GAME_REST;
    g->pending = g->taps = g->step = g->trick = 0;
    g->finger_down = g->circle_valid = false;
    g->hit_valid = false;
    g->pulse = 0;
    g->trick_wait = TRICK_INTERVAL;
}
void tess_games_set_available(face_t *f, bool available) {
    f->tess_games.available = available;
    if (!available) leave(&f->tess_games);
}

static unsigned skill(const tess_games_t *g) { return (g->game == TESS_GAME_CATCH ? 3u : 0u) + g->tier; }
static unsigned next_tier(const tess_games_t *g, unsigned game) {
    unsigned base = game == TESS_GAME_CATCH ? 3 : 0;
    for (unsigned tier = 0; tier < 3; tier++)
        if (!(g->progress & (1u << (base + tier)))) return tier;
    return 2;  // mastered games remain playable; repeating never adds progress
}
bool tess_games_start(face_t *f, tess_game_t game, unsigned tier) {
    if (!eligible(f) || (game != TESS_GAME_ECHO && game != TESS_GAME_CATCH) || tier > 2) return false;
    tess_games_t *g = &f->tess_games;
    leave(g);
    if (!++g->round) g->round = 1;
    g->game = game;
    g->phase = TESS_GAME_SHOW;
    g->tier = (uint8_t)tier;
    g->age = g->clock = 0;
    g->last_tap = 99;
    g->hit_valid = false;
    // The invitation's old follow-through must not replay after the game.
    memset(f->tess_play.after_what, 0xff, sizeof f->tess_play.after_what);
    f->tess_play.cue_n = f->tess_play.invites = 0;
    f->tess_play.waiting = f->tess_play.dodge_t = 0;
    f->tess_play.down = false;
    f->tess_taps = 0;
    f->rub = (rub_t){0};
    f->rub_love = false;
    f->tess_rigid = 0;
    return true;
}

static const float echo_at[3][4] = {{0, .60f, 0, 0}, {0, .45f, 1.20f, 0}, {0, .45f, .90f, 1.80f}};
static void won(tess_games_t *g) {
    g->progress |= (uint8_t)(1u << skill(g));
    g->phase = TESS_GAME_CELEBRATE;
    g->clock = 0;
    g->pulse = 1;
    g->trick = (uint8_t)(skill(g) + 1);
    g->trick_age = 0;
}
static void echo_tap(tess_games_t *g) {
    if (g->phase != TESS_GAME_WAIT) return;
    if (g->step) {
        float interval = echo_at[g->tier][g->step] - echo_at[g->tier][g->step - 1];
        if (fabsf(g->last_tap - interval) > .24f) { leave(g); return; }
    }
    g->step++;
    g->last_tap = 0;
    g->pulse = 1;
    if (g->step == g->tier + 2) won(g);
    else g->deadline = echo_at[g->tier][g->step] - echo_at[g->tier][g->step - 1] + .4f;
}
static void catch_tap(tess_games_t *g, float x, float y) {
    if (g->phase != TESS_GAME_WAIT || !g->hit_valid) return;
    if (g->last_tap < .6f) return;
    if (g->step && hypotf(g->hit[0] - g->last_hit[0], g->hit[1] - g->last_hit[1]) < 48.f) return;
    if (hypotf(x - g->hit[0], y - g->hit[1]) > 44.f) { leave(g); return; }
    memcpy(g->last_hit, g->hit, sizeof g->hit);
    g->step++;
    g->last_tap = 0;
    g->pulse = 1;
    g->deadline = 6;
    if (g->step == g->tier + 3) won(g);
}
static void tap_trigger(tess_games_t *g, float x, float y) {
    bool close = hypotf(x - g->tap_x, y - g->tap_y) <= 45.f;
    bool rhythm = g->last_tap >= .16f && g->last_tap <= .65f;
    g->taps = close && rhythm ? (uint8_t)(g->taps < 6 ? g->taps + 1 : 6) : 1;
    g->tap_x = x; g->tap_y = y;
    g->last_tap = 0;
    g->pending = g->taps == 3 ? TESS_GAME_ECHO : TESS_GAME_NONE;
}
static bool panel_position(float x, float y) {
    return x >= 0 && x < 480 && y >= 0 && y < 480;
}
bool tess_games_event(face_t *f, face_event_t event, float x, float y) {
    tess_games_t *g = &f->tess_games;
    bool allowed = eligible(f);
    if (!allowed) leave(g);
    if (event == FEV_TAP && !panel_position(x, y)) {
        if (!g->game) leave(g);
        return true;  // malformed coordinates must never reach ordinary tap reactions
    }
    if (!allowed) return false;
    if (event != FEV_TAP) {
        if (g->game || event != FEV_PET) leave(g);
        else { g->pending = g->taps = g->trick = 0; g->trick_wait = TRICK_INTERVAL; }
        return false;  // a hold/stroke exits into ordinary petting, shake still scatters
    }
    if (g->phase == TESS_GAME_CELEBRATE) { leave(g); return false; }
    if (g->game) {
        if (g->game == TESS_GAME_ECHO) echo_tap(g);
        else catch_tap(g, x, y);
        return true;  // game taps never become agent pokes or tap-series agitation
    }
    g->trick = 0;
    g->trick_wait = TRICK_INTERVAL;
    tap_trigger(g, x, y);
    return false;  // keep the immediate, familiar response to the trigger's taps
}

static void circle_sample(tess_games_t *g, float x, float y) {
    float radius = hypotf(x - 240, y - 255), angle = atan2f(y - 255, x - 240);
    float delta = remainderf(angle - g->circle_angle, 2 * PI);
    g->circle_angle = angle;
    // A short rendering stall can skip several 50Hz touch samples. Keep a
    // directed quarter-turn of slack; larger jumps still invalidate the path.
    if (radius < 55 || radius > 175 || fabsf(delta) > 1.6f) { g->circle_valid = false; return; }
    if (g->circle_sweep * delta < 0 && fabsf(delta) > .12f) { g->circle_valid = false; return; }
    g->circle_sweep += delta;
}
static bool closed_circle(const tess_games_t *g, const rub_input_t *in) {
    return g->circle_valid && g->finger_age >= .6f && g->finger_age <= 4.f &&
        fabsf(g->circle_sweep) >= 5.5f && hypotf(in->x - g->finger_x, in->y - g->finger_y) < 65;
}
static void begin_pointer(tess_games_t *g, const rub_input_t *in) {
    g->finger_age = g->circle_sweep = 0;
    g->finger_x = in->x; g->finger_y = in->y;
    g->circle_angle = atan2f(in->y - 255, in->x - 240);
    float radius = hypotf(in->x - 240, in->y - 255);
    g->circle_valid = radius >= 55 && radius <= 175;
}
static void pointer(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    const rub_input_t *in = &f->rub_in;
    if (in->down && !g->game) { g->trick = 0; g->trick_wait = TRICK_INTERVAL; }
    if (in->down && !g->finger_down) {
        begin_pointer(g, in);
    } else if (in->down) {
        g->finger_age += dt;
        if (g->circle_valid) circle_sample(g, in->x, in->y);
        if (g->game && g->finger_age >= .65f) leave(g);
    } else if (g->finger_down && !g->game) {
        if (closed_circle(g, in)) { g->pending = TESS_GAME_CATCH; g->last_tap = .8f; }
    }
    g->finger_down = in->down;
}

static void advance_round(tess_games_t *g, float dt) {
    g->age += dt;
    g->clock += dt;
    if (g->age >= ROUND_LIMIT) { leave(g); return; }
    if (g->phase == TESS_GAME_CELEBRATE) {
        if (g->clock >= 3.f) leave(g);
        return;
    }
    if (g->phase == TESS_GAME_WAIT) {
        g->deadline -= dt;
        if (g->deadline <= 0) leave(g);
        return;
    }
    if (g->game == TESS_GAME_ECHO) {
        unsigned beats = g->tier + 2;
        if (g->step < beats && g->clock >= .6f + echo_at[g->tier][g->step]) { g->step++; g->pulse = 1; }
        if (g->clock < .6f + echo_at[g->tier][beats - 1] + .6f) return;
    } else if (g->clock < 1.f) return;
    g->phase = TESS_GAME_WAIT;
    g->step = 0;
    g->clock = 0;
    g->deadline = g->game == TESS_GAME_ECHO ? 5.f : 6.f;
}
static float approach(float value, float target, float step) {
    return value + clampf(target - value, -step, step);
}
static bool quiet(const face_t *f) {
    return !f->rub_in.down && f->rub.stage == 0 && f->tess_hold[TR_HEART] <= 0 &&
        f->tess_hold[TR_SCATTER] <= 0 && f->tess_agitation < .2f;
}
static void choose_trick(tess_games_t *g) {
    for (unsigned n = 0; n < 6; n++) {
        unsigned index = (g->next_trick + n) % 6;
        if (!(g->progress & (1u << index))) continue;
        g->trick = (uint8_t)(index + 1);
        g->next_trick = (uint8_t)((index + 1) % 6);
        g->trick_age = 0;
        break;
    }
    g->trick_wait = TRICK_INTERVAL;
}
static void trick_pose(tess_games_t *g, float dt, float *fold, float goal[2]) {
    g->trick_age += dt;
    float t = g->trick_age;
    if (t >= 3.f) { g->trick = 0; return; }
    float envelope = sinf(PI * t / 3.f);
    if (g->trick <= 3) {
        float beat = sinf((g->trick + 1) * PI * t / 3.f);
        g->pulse = fmaxf(g->pulse, beat * beat * envelope);
        if (g->trick >= 2) goal[1] = -.3f * envelope;
        if (g->trick == 3) *fold = .5f * envelope;
    } else if (g->trick == 6) *fold = envelope;
    else {
        goal[0] = .65f * sinf(t * 2 * PI / 3) * envelope;
        goal[1] = .35f * sinf(t * (g->trick == 5 ? 4 : 2) * PI / 3) * envelope;
    }
}
static void animate(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    bool conversation = f->live_active || f->mode == MODE_THINKING || f->mode == MODE_SPEAKING;
    float wanted = conversation ? 3 : grown_form(g->progress);
    g->form = approach(g->form, wanted, dt * 1.2f);
    float fold = 0, goal[2] = {0, 0};
    if (g->game == TESS_GAME_CATCH && g->phase != TESS_GAME_CELEBRATE) {
        float angle = g->age * (.45f + .18f * g->tier) + g->step * 1.7f;
        fold = 1;
        goal[0] = 1.2f * cosf(angle); goal[1] = .9f * sinf(angle);
    }
    if (g->trick) trick_pose(g, dt, &fold, goal);
    g->fold = approach(g->fold, fold, dt * 2.5f);
    for (int k = 0; k < 2; k++) {
        g->offset[k] += (goal[k] - g->offset[k]) * fminf(1, dt * 5.f);
        if (fabsf(g->offset[k]) < .0001f && goal[k] == 0) g->offset[k] = 0;
    }
}
static void advance_activity(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    pointer(f, dt);
    if (g->pending && !g->finger_down && g->last_tap >= TAP_PAUSE) {
        unsigned game = g->pending;
        tess_games_start(f, (tess_game_t)game, next_tier(g, game));
    }
    if (g->game) advance_round(g, dt);
    else if (!g->pending && quiet(f)) {
        g->trick_wait -= dt;
        if (g->trick_wait <= 0 && !g->trick) choose_trick(g);
    }
}
void tess_games_update(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    if (!g->ready || face_character(f) != CHARACTER_TESS || !isfinite(dt) || dt <= 0) return;
    dt = fminf(dt, .1f);
    bool allowed = eligible(f);
    if (!allowed) leave(g);
    if (!visible(f)) return;  // preemption above still runs when rendering is paused
    g->last_tap = fminf(99, g->last_tap + dt);
    g->pulse = fmaxf(0, g->pulse - dt * 4.f);
    if (allowed) advance_activity(f, dt);
    animate(f, dt);
}
