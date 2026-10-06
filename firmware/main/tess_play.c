#include "tess_internal.h"

#include <string.h>

// Play. What follows from what was done to Tess, and what it does when nobody does anything.
// A queue of reactions that come later (tess_after) makes a reaction a little story: the look first, then the
// action, then the follow-through (a swing back, a peek, a wiggle for more). Alone, it tries to be noticed: a
// wiggle, a peek, a bow; then it waits, and asks again, less hopefully. Ignored longer, it grows bored and up to
// mischief: it looks away and turns back with a pop, whirls, hides a cell (a quarter turn through the 4th
// dimension) and comes back. A finger that answers an invitation, or catches it after a dodge, makes it glad;
// after being petted it settles into a slow contented sway (tess_touch.c). Everything is a turn, a carried shift or a colour.
#define INVITE_LO 25.f     // s alone before the first invitation (random up to INVITE_HI), and between the later ones
#define INVITE_HI 40.f
#define AGAIN_LO 30.f
#define AGAIN_HI 55.f
#define WAIT_S 5.5f        // s it waits for an answer
#define SLEEPY_SPAN 100.f  // s until its invitations have lost as much vigour as they do, from 20 s alone on

void tess_play_reset(face_t *f) {
    tess_play_t *p = &f->tess_play;
    memset(p, 0, sizeof *p);
    memset(p->after_what, AFTER_NONE, sizeof p->after_what);
    p->invite_in = INVITE_LO;
    p->up_t = 9;  // no finger yet
}

static bool cues_quiet(const face_t *f) {
    return f->dark || f->mode == MODE_SLEEP || (f->emotion == EMO_SLEEPY && !f->tess_play.down);
}

void tess_cue(face_t *f, tess_cue_t cue, float strength, float position) {
    tess_play_t *p = &f->tess_play;
    if (cues_quiet(f) || p->cue_n >= TESS_CUES) return;
    f->feel.cue_age = 0;
    p->cue[p->cue_n].cue = (uint8_t)cue;
    p->cue[p->cue_n].strength = strength;
    p->cue[p->cue_n++].position = position;
}

bool face_take_cue(face_t *f, tess_cue_t *cue, float *strength, float *position) {
    tess_play_t *p = &f->tess_play;
    if (cues_quiet(f)) p->cue_n = 0;
    if (!p->cue_n) return false;
    *cue = (tess_cue_t)p->cue[0].cue;
    *strength = p->cue[0].strength;
    *position = p->cue[0].position;
    memmove(p->cue, p->cue + 1, --p->cue_n * sizeof p->cue[0]);
    return true;
}

void tess_after(face_t *f, float seconds, int what, float strength) {
    tess_play_t *p = &f->tess_play;
    for (int i = 0; i < TESS_QUEUE; i++) {
        if (p->after_what[i] != AFTER_NONE) continue;
        p->after_t[i] = seconds;
        p->after_s[i] = strength;
        p->after_what[i] = (uint8_t)what;
        return;
    }
}

void tess_play_glad(face_t *f) {
    tess_feel_event(f, ME_GLAD, .8f, 0);
    tess_react(f, TR_JOY, 1.6f);
    tess_kick(f, KICK_HOP, 1.f);
    tess_kick(f, KICK_TWIRL, .8f);
    tess_cue(f, TC_EXCITE, .8f, 0);
}

static void reset_alone(face_t *f) {
    tess_play_t *p = &f->tess_play;
    p->alone = p->waiting = 0;
    p->invites = 0;
    p->invite_in = frange(f, INVITE_LO, INVITE_HI) / f->style.invite;
}

bool tess_play_touched(face_t *f) {
    tess_play_t *p = &f->tess_play;
    bool glad = p->waiting > 0 || p->dodge_t > 0;  // it asked, or it was caught: that is what it wanted
    p->dodge_t = 0;
    reset_alone(f);
    if (glad) tess_play_glad(f);
    return glad;
}

// (a touch has been noticed when the finger landed, see tess_touch.c)
void tess_play_event(face_t *f, face_event_t event) {
    if (event == FEV_TAP || event == FEV_PET || event == FEV_SHAKE || event == FEV_PICKUP || event == FEV_WAKE) reset_alone(f);
}

// The turns that follow on. Signed strengths carry the direction: a swing goes back the way it came.
static void run_after(face_t *f, int what, float s) {
    tess_play_t *p = &f->tess_play;
    tess_mood_t *m = &f->tess_mood;
    float side = s < 0 ? -1.f : 1.f;
    switch (what) {
    case AFTER_SWING:
        m->twirl += side * (4.5f + 2.5f * fabsf(s));
        tess_cue(f, TC_SWING, fabsf(s), side);
        break;
    case AFTER_UNTURN:  // back from hiding
        m->yw_goal += s;
        break;
    case AFTER_PEEK:
        tess_gaze_peek(f);
        tess_cue(f, TC_PEEK, 0, 0);
        break;
    case AFTER_LOOK_AWAY:
        p->hold[0] = 1.6f * s; p->hold[1] = p->hold[2] = 0;
        p->hold_t = .7f;
        tess_let_go(m, .9f);
        tess_cue(f, TC_GLANCE, 0, side);
        break;
    case AFTER_BOO:  // turns round with a pop
        p->hold_t = 0;
        tess_react(f, TR_SURPRISE, 0);
        tess_kick(f, KICK_HOP, 1.f);
        tess_kick(f, KICK_TWIRL, 1.f);
        tess_gaze_notice(f, 6.f);
        break;
    case AFTER_CONTENT:
        p->content_t = TESS_CONTENT_S;
        tess_cue(f, TC_CONTENT, 0, 0);
        break;
    case AFTER_JOY_CUE:
        tess_cue(f, TC_JOY, 1.f, 0);
        break;
    default:
        tess_kick(f, what, s);
        break;
    }
}

static void run_queue(face_t *f, float dt) {
    tess_play_t *p = &f->tess_play;
    for (int i = 0; i < TESS_QUEUE; i++) {
        if (p->after_what[i] == AFTER_NONE || (p->after_t[i] -= dt) > 0) continue;
        int what = p->after_what[i];
        p->after_what[i] = AFTER_NONE;
        run_after(f, what, p->after_s[i]);
    }
}

// "Hey!": one of three ways to ask for a look, then it waits, and asks once more halfway.
static void invite(face_t *f) {
    tess_play_t *p = &f->tess_play;
    float vigour = 1 - .4f * tess_quiet(p->alone, SLEEPY_SPAN), side = frand(f) < .5f ? -1.f : 1.f;
    tess_cue(f, TC_INVITE, vigour, side);
    tess_gaze_notice(f, WAIT_S + 2.f);
    switch ((int)(frand(f) * 3) % 3) {
    case 0:  // a wiggle and a hop
        tess_kick(f, KICK_WIGGLE, .9f * vigour);
        tess_after(f, .3f, KICK_HOP, .7f * vigour);
        break;
    case 1:  // looks away, then peeks back through the 4th dimension
        tess_after(f, 0, AFTER_LOOK_AWAY, side * .5f);
        tess_after(f, .8f, AFTER_PEEK, 0);
        break;
    default:  // a bow and a twirl
        tess_kick(f, KICK_NOD, .8f * vigour);
        tess_after(f, .35f, KICK_TWIRL, .6f * vigour);
        break;
    }
    tess_after(f, 2.8f, KICK_WIGGLE, .45f * vigour);  // "hello?"
    p->waiting = WAIT_S;
}

// Bored: a trick. It looks away, keeps quite still, and turns back with a pop; or whirls; or hides a cell and comes back.
static void mischief(face_t *f) {
    tess_play_t *p = &f->tess_play;
    tess_mood_t *m = &f->tess_mood;
    float side = frand(f) < .5f ? -1.f : 1.f;
    tess_cue(f, TC_MISCHIEF, 0, side);
    switch ((int)(frand(f) * 3) % 3) {
    case 0:
        p->hold[0] = side * .6f; p->hold[1] = p->hold[2] = 0;
        p->hold_t = 1.8f;
        tess_let_go(m, 2.f);
        tess_after(f, 1.8f, AFTER_BOO, 0);
        break;
    case 1:
        m->barrel_goal += side * 2 * PI;
        m->twirl += side * 6.f;
        tess_after(f, .9f, KICK_HOP, .8f);
        break;
    default:  // hides a cell, and pops back
        m->yw_goal += side * PI / 2;
        tess_after(f, 1.5f, AFTER_UNTURN, -side * PI / 2);
        tess_after(f, 1.5f, AFTER_BOO, 0);
        tess_cue(f, TC_PEEK, 0, 0);
        break;
    }
    p->invites = 0;
}

static bool alone_and_calm(const face_t *f) {
    return f->mode == MODE_IDLE && f->emotion != EMO_SLEEPY && !f->tess_play.down && !f->rub.stage && f->tess_hold[TR_HEART] <= 0 &&
           f->tess_hold[TR_SCATTER] <= 0 && f->tess_agitation < .2f && f->tess_mode[TM_SLEEP] < .3f;
}

void tess_play_update(face_t *f, float dt) {
    tess_play_t *p = &f->tess_play;
    run_queue(f, dt);
    p->hold_t = fmaxf(0, p->hold_t - dt);
    p->dodge_t = fmaxf(0, p->dodge_t - dt);
    p->dodge_cool = fmaxf(0, p->dodge_cool - dt);
    p->run_t = fmaxf(0, p->run_t - dt);
    p->content_t = fmaxf(0, p->content_t - dt);
    bool playing = f->tess_mood.free_t > 0;
    p->keep += ((playing ? 1.f : 0.f) - p->keep) * fminf(1, dt * (playing ? 20.f : .6f));  // it lets the pose go home slowly
    if (!alone_and_calm(f)) return;
    p->alone += dt;
    if (p->waiting > 0) {
        if ((p->waiting -= dt) <= 0 && ++p->invites == 2) tess_feel_event(f, ME_IGNORED, .6f, 0);  // nobody came
        return;
    }
    if ((p->invite_in -= dt) > 0) return;
    p->invite_in = frange(f, AGAIN_LO, AGAIN_HI) / f->style.invite;
    if (p->invites >= 2) mischief(f);
    else invite(f);
}

// The pose it asks for while it keeps still (a held look, a freeze) or plays (it lets go of facing you, and the
// tip and turn of the moment stay where they are rather than going home).
void tess_play_targets(face_t *f, float to[TP_COUNT], float *spin) {
    const tess_play_t *p = &f->tess_play;
    const tess_mood_t *m = &f->tess_mood;
    to[TP_YAW] += p->keep * (m->pose[TP_YAW] - to[TP_YAW]);
    to[TP_PITCH] += p->keep * (m->pose[TP_PITCH] - to[TP_PITCH]);
    if (p->hold_t <= 0) return;
    to[TP_YAW] = p->hold[0];
    to[TP_PITCH] = p->hold[1];
    *spin = p->hold[2];
}
