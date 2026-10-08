#include "tess_internal.h"
#include "face_math.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265f
#define INVITE_DELAY .55f
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
    g->finger_down = g->invited = false;
    g->trail_n = 0;
    g->trail_at = 0;
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
    g->reply_delay = .45f;
    g->tap_x = 240; g->tap_y = 255;
    g->feedback_age = 1;
    g->waypoint = (uint8_t)(tier * 2);
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

// A reply follows the player's timing and position, rather than asking them
// to discover and memorise an invisible metronome.
static void answer(face_t *f, tess_cue_t cue, float strength) {
    tess_games_t *g = &f->tess_games;
    g->pulse = 1;
    tess_touch_wave(f, g->tap_x, g->tap_y);
    tess_cue(f, cue, strength, clampf((g->tap_x - 240) / 240, -1, 1));
}
static void won(face_t *f) {
    tess_games_t *g = &f->tess_games;
    g->progress |= (uint8_t)(1u << skill(g));
    g->phase = TESS_GAME_CELEBRATE;
    g->clock = 0;
    g->trick = (uint8_t)(skill(g) + 1);
    g->trick_age = 0;
    answer(f, TC_JOY, .65f);
}
static void echo_tap(tess_games_t *g, float x, float y) {
    if (g->phase != TESS_GAME_WAIT || g->last_tap < .35f) return;
    g->step++;
    g->tap_x = x; g->tap_y = y;
    g->reply_delay = clampf(g->last_tap * .35f, .24f, .48f);
    g->last_tap = g->clock = 0;
    g->phase = TESS_GAME_SHOW;
}
static void next_destination(tess_games_t *g) {
    // Unequal flights and pauses: it escapes, slows down, and waits to be caught.
    static const float places[7][2] = {{1.25f,-.7f},{-.65f,.9f},{-1.3f,-.45f},
        {.6f,.7f},{.05f,-1.f},{1.1f,.35f},{-.9f,.25f}};
    g->waypoint = (uint8_t)((g->waypoint + 3 + g->tier) % 7);
    memcpy(g->destination, places[g->waypoint], sizeof g->destination);
    g->flight = 1.4f + .18f * (g->waypoint % 3) - .15f * g->tier;
}
static void catch_tap(face_t *f, float x, float y) {
    tess_games_t *g = &f->tess_games;
    if (g->phase != TESS_GAME_WAIT || !g->hit_valid || g->last_tap < .45f) return;
    g->tap_x = x; g->tap_y = y;
    if (hypotf(x - g->hit[0], y - g->hit[1]) > 68.f) {
        // A miss is still a conversation: lean towards the hand and offer a
        // closer catch. No discovery, no invisible restart, no punishment.
        g->destination[0] = clampf((x - 240) / 100, -1.25f, 1.25f);
        g->destination[1] = clampf((y - 255) / 100, -.9f, .9f);
        g->flight = 1.8f;
        g->deadline = 6;
        g->last_tap = 0;
        answer(f, TC_DODGE, .35f);
        return;
    }
    if (g->step && hypotf(g->hit[0] - g->last_hit[0], g->hit[1] - g->last_hit[1]) < 48.f) return;
    memcpy(g->last_hit, g->hit, sizeof g->hit);
    g->step++;
    g->last_tap = 0;
    g->deadline = 6;
    if (g->step == g->tier + 3) won(f);
    else { answer(f, TC_EXCITE, .3f + .1f * g->step); next_destination(g); }
}
static void tap_trigger(face_t *f, float x, float y) {
    tess_games_t *g = &f->tess_games;
    if (g->invited && g->last_tap >= INVITE_DELAY && g->last_tap < 6.f) {
        tess_games_start(f, TESS_GAME_ECHO, next_tier(g, TESS_GAME_ECHO));
        g->phase = TESS_GAME_WAIT;
        echo_tap(g, x, y);
        return;
    }
    bool rapid = g->last_tap < INVITE_DELAY;
    g->taps = rapid ? (uint8_t)(g->taps < 6 ? g->taps + 1 : 6) : 1;
    g->tap_x = x; g->tap_y = y;
    g->last_tap = 0;
    g->invited = false;
    g->pending = g->taps == 1 ? TESS_GAME_ECHO : TESS_GAME_NONE;
}
static void acknowledge(face_t *f, float x, float y) {
    tess_games_t *g = &f->tess_games;
    if (g->feedback_age < .18f) return;
    g->feedback_age = 0;
    g->pulse = fmaxf(g->pulse, .25f);
    tess_cue(f, TC_TOUCH, .12f, (x - 240) / 240);
    (void)y;
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
        else { g->pending = g->taps = g->trick = 0; g->invited = false; g->trick_wait = TRICK_INTERVAL; }
        return false;  // a hold/stroke exits into ordinary petting, shake still scatters
    }
    if (g->phase == TESS_GAME_CELEBRATE) { leave(g); return false; }
    if (g->game) {
        acknowledge(f, x, y);
        if (g->game == TESS_GAME_ECHO) echo_tap(g, x, y);
        else catch_tap(f, x, y);
        return true;  // game taps never become agent pokes or tap-series agitation
    }
    g->trick = 0;
    g->trick_wait = TRICK_INTERVAL;
    tap_trigger(f, x, y);
    return g->game != TESS_GAME_NONE;  // the first tap is ordinary; accepting the invitation is local
}

static void begin_pointer(tess_games_t *g, const rub_input_t *in) {
    g->finger_age = 0;
    g->finger_x = in->x; g->finger_y = in->y;
    g->finger_cancelled = false;
}
static void release_invitation(tess_games_t *g, const rub_input_t *in) {
    float distance = hypotf(in->x - g->finger_x, in->y - g->finger_y);
    if (g->finger_age < .08f || g->finger_age >= .6f || distance < 72.f) return;
    g->pending = TESS_GAME_CATCH;
    g->last_tap = 0;
    g->invited = false;
}
static void moving_pointer(face_t *f, float dt, const rub_input_t *in) {
    tess_games_t *g = &f->tess_games;
    g->finger_age += dt;
    float distance = hypotf(in->x - g->finger_x, in->y - g->finger_y);
    if (g->finger_age < .65f && distance < 18.f) return;
    if (!g->game) { g->pending = 0; g->invited = false; return; }
    float x = g->finger_x, y = g->finger_y, age = g->finger_age;
    leave(g);
    g->finger_x = x; g->finger_y = y; g->finger_age = age;
    g->finger_cancelled = true;
    tess_touch_resume(f, x, y, age);
}
static void pointer(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    const rub_input_t *in = &f->rub_in;
    if (in->down && !g->game) { g->trick = 0; g->trick_wait = TRICK_INTERVAL; }
    if (in->down && !g->finger_down) begin_pointer(g, in);
    else if (in->down) moving_pointer(f, dt, in);
    else if (g->finger_down && !g->game && !g->finger_cancelled) release_invitation(g, in);
    g->finger_down = in->down;
}
static void advance_round(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
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
    if (g->clock < g->reply_delay) return;
    if (g->game == TESS_GAME_ECHO) {
        if (g->step >= 4 + 2 * g->tier) { won(f); return; }
        answer(f, g->step % 2 ? TC_EXCITE : TC_SWING, .35f + .07f * g->tier);
    } else { next_destination(g); answer(f, TC_SWING, .25f); }
    g->phase = TESS_GAME_WAIT;
    g->clock = 0;
    g->deadline = 6;
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
static void game_pose(tess_games_t *g, float dt, float *wanted, float *fold, float goal[2]) {
    if (g->game == TESS_GAME_CATCH && g->phase != TESS_GAME_CELEBRATE) {
        *fold = 1;
        g->flight -= dt;
        if (g->flight <= 0 && g->phase == TESS_GAME_WAIT) next_destination(g);
        memcpy(goal, g->destination, sizeof g->destination);
    } else if (g->game == TESS_GAME_ECHO || g->invited) {
        // Each answer travels from the touched side, opens, and bows back.
        *wanted = fmaxf(*wanted, 1.f);
        goal[0] = clampf((g->tap_x - 240) / 240, -1, 1) * (.55f + .1f * g->tier) * g->pulse;
        goal[1] = (clampf((g->tap_y - 255) / 240, -1, 1) * .4f - .3f - .08f * g->tier) * g->pulse;
        *fold = (g->step % 2 ? .15f : .4f) * (1 + .25f * g->tier) * g->pulse;
    }
}
static void animate(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    bool conversation = f->live_active || f->mode == MODE_THINKING || f->mode == MODE_SPEAKING;
    float wanted = conversation ? 3 : grown_form(g->progress);
    float fold = 0, goal[2] = {0, 0};
    game_pose(g, dt, &wanted, &fold, goal);
    g->form = approach(g->form, wanted, dt * 1.2f);
    if (g->trick) trick_pose(g, dt, &fold, goal);
    g->fold = approach(g->fold, fold, dt * 2.5f);
    for (int k = 0; k < 2; k++) {
        g->offset[k] += (goal[k] - g->offset[k]) * fminf(1, dt * (g->game == TESS_GAME_CATCH ? 3.5f : 5.f));
        if (fabsf(g->offset[k]) < .0001f && goal[k] == 0) g->offset[k] = 0;
    }
}
static void advance_activity(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    pointer(f, dt);
    if (g->pending && !g->finger_down && g->last_tap >= INVITE_DELAY) {
        if (g->pending == TESS_GAME_CATCH) tess_games_start(f, TESS_GAME_CATCH, next_tier(g, TESS_GAME_CATCH));
        else {
            g->pending = 0;
            g->invited = true;
            answer(f, TC_SWING, .3f);
        }
    }
    if (g->invited && g->last_tap >= 6.f) g->invited = false;
    if (g->game) advance_round(f, dt);
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
    g->feedback_age = fminf(1, g->feedback_age + dt);
    g->pulse = fmaxf(0, g->pulse - dt * 4.f);
    if (allowed) advance_activity(f, dt);
    animate(f, dt);
}
