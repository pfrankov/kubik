#include "tess_internal.h"

#include <string.h>

// How the cube carries itself. Idle is a run of short moves, each picked at random with a length of its own, so
// the timing never repeats: a drift (the turn's speed and direction change, and so does the axis it tips about),
// a rest that eases to near-stillness, a curious glance, a small wobble and, now and then, a small turn in 4D.
// That one is in the y-w plane, which waiting never uses (waiting turns x-w and z-w round and round): a quarter
// turn of the inner cube, which then rests there: the tesseract maps onto itself every quarter, so it
// never holds still half way. A move only sets targets; the offsets ease towards them on
// springs and the turn's rate on a lag, so pose and speed stay continuous. What the sensors already sampled say
// (stillness, battery, touch, shakes) shifts the odds and the vigour. Talking states set their own targets;
// waiting sets none, so it turns exactly as before.
enum { MV_DRIFT, MV_REST, MV_GLANCE, MV_WOBBLE, MV_TURN4D, MV_ORBIT, MV_PEEK, MV_COUNT };
#define QUIET_SPAN 100.f   // s until it is as sleepy as it gets, from 20 s without touch or motion (the app's own drowsy face comes at 120 s)
#define WAITING_SPIN .22f  // rad/s: the turn every other state keeps

void tess_mood_reset(face_t *f) {
    tess_mood_t *m = &f->tess_mood;
    memset(m, 0, sizeof *m);
    m->spin = WAITING_SPIN;
    m->len = m->run = 1;  // the first idle update picks a move
    m->next_4d = 10;
    m->attentive = true;  // it starts by looking at you
    m->phase_left = 6.f;
    m->cell = 4;          // its face: the +z cell
}

static void start_move(face_t *f, int move) {
    tess_mood_t *m = &f->tess_mood;
    float side = frand(f) < .5f ? -1.f : 1.f;
    m->move = (uint8_t)move;
    m->run = 0;
    switch (move) {
    case MV_DRIFT: m->len = frange(f, 4.f, 9.f); m->amp = side * frange(f, .12f, .42f); m->aux = frange(f, -.16f, .16f); break;
    case MV_REST: m->len = frange(f, 2.5f, 6.f); m->amp = side * frange(f, .01f, .05f); m->aux = 0; break;
    case MV_GLANCE: m->len = frange(f, 2.4f, 3.6f); m->amp = side * frange(f, .3f, .55f); m->aux = frange(f, -.12f, .1f); break;
    case MV_WOBBLE: m->len = frange(f, 2.2f, 3.4f); m->amp = side * frange(f, .08f, .2f); m->aux = side * frange(f, .12f, .2f); break;
    case MV_ORBIT: m->len = frange(f, 7.f, 10.f); m->amp = side * frange(f, .4f, .65f); m->aux = frange(f, .08f, .16f); break;
    case MV_PEEK: m->len = frange(f, 3.6f, 5.f); m->amp = side * frange(f, .3f, .42f); m->aux = side * frange(f, .06f, .12f); break;
    default:
        m->len = frange(f, 6.f, 8.5f); m->amp = m->aux = 0; m->next_4d = frange(f, 25.f, 55.f) / f->style.turn4d;
        m->w_goal[2] += side * PI / 2;
        break;
    }
}

// Odds of each move. The tired and the flat-battery rest more; touched, it glances about; charging, it sways
// contentedly; a 4D turn is allowed once its own timer has run out.
static int pick_move(face_t *f, float sleepy, bool low) {
    tess_mood_t *m = &f->tess_mood;
    float awake = 1 - sleepy;
    float odds[MV_COUNT] = {
        [MV_DRIFT] = 5.f,
        [MV_REST] = (1.5f + 5.5f * sleepy + (low ? 3.f : 0.f)) * f->style.rest,
        [MV_GLANCE] = 2.5f * awake * (1 + 2 * m->perk),
        [MV_WOBBLE] = (1.5f + (f->charging ? 2.f : 0.f)) * awake,
        [MV_TURN4D] = m->next_4d <= 0 && !low && !m->shaken && !m->attentive ? 8.f * awake * f->style.turn4d : 0.f,  // daydreaming: while its attention wanders
        [MV_ORBIT] = 1.6f * awake * (low ? .4f : 1.f),  // the camera circles the cube: a look from every side
        [MV_PEEK] = 1.8f * awake,                       // the camera dips low or climbs high for a look from below or above
    };
    odds[m->move] *= .25f;  // hardly ever the same one twice
    float total = 0;
    for (int i = 0; i < MV_COUNT; i++) total += odds[i];
    float r = frand(f) * total;
    int pick = 0;
    while (pick < MV_COUNT - 1 && r >= odds[pick]) r -= odds[pick++];
    return pick;
}

// Targets of the idle moves: `u` is how far into the move it is.
static void idle_targets(const face_t *f, float sleepy, bool low, float to[TP_COUNT], float *spin) {
    const tess_mood_t *m = &f->tess_mood;
    float u = m->run / m->len, hold = smooth01(u * 8.f) * (1 - smooth01((u - .65f) * 6.f));  // in, held, out
    float vigor = (1 - .65f * sleepy) * (low ? .5f : 1.f) * (1 + .6f * m->perk);
    float env = tess_sin(PI * u);
    *spin = m->amp * vigor;
    switch (m->move) {
    case MV_DRIFT: to[TP_PITCH] = m->aux * vigor; to[TP_ROLL] = -.6f * m->aux * vigor; break;
    case MV_GLANCE: to[TP_YAW] = hold * m->amp * vigor; to[TP_PITCH] = hold * m->aux * vigor; *spin *= .3f; break;
    case MV_WOBBLE: to[TP_ROLL] = m->aux * vigor * env * env * tess_sin(m->run * 4.8f); break;
    case MV_TURN4D: *spin *= .1f; break;
    case MV_ORBIT: to[TP_PITCH] = env * m->aux * vigor; *spin *= 1.f + .4f * env; break;
    case MV_PEEK: to[TP_PITCH] = hold * m->amp * vigor; to[TP_ROLL] = hold * m->aux * vigor; *spin *= .25f; break;
    default: break;
    }
    // Shaken hard: giddy for a moment, in every direction at once (a small shake only trembles the dots, tess_motion.c).
    float giddy = smooth01((m->dizzy - .5f) / .3f);
    to[TP_PITCH] += giddy * .35f * tess_sin(f->t * 4.5f);
    to[TP_ROLL] += giddy * .4f * tess_sin(f->t * 5.5f + 1.f);
}

// Talking: it faces the speaker, leans in to the voice; speaking, it pulses with it.
static void context_targets(const face_t *f, float to[TP_COUNT], float *spin) {
    float audio = f->tess_audio;
    *spin = WAITING_SPIN;
    if (f->mode == MODE_LISTENING) {
        *spin = .12f;
        to[TP_PITCH] = -.05f - .1f * audio;
    } else if (f->mode == MODE_SPEAKING) {
        *spin = .3f + .5f * audio;
        to[TP_PITCH] = -.28f * audio;
        to[TP_ROLL] = .14f * audio * tess_sin(f->t * 2.6f);
    }
}

// Reactions move the body as a whole: a shove of the carried offset (hop, flinch, nuzzle), a turn about the
// vertical (twirl), a nod or a roll wobble. Nothing bends: a point never moves except with all the others.
void tess_mood_kick(face_t *f, int kind, float s, float tx, float ty) {
    tess_mood_t *m = &f->tess_mood;
    float side = frand(f) < .5f ? -1.f : 1.f;
    float dx = clampf((tx - 240) / 150, -1, 1), dy = clampf((ty - 255) / 150, -1, 1);
    switch (kind) {
    case KICK_HOP: m->shift_rate[1] -= (3.2f + .8f * frand(f)) * s; break;  // boing
    case KICK_WIGGLE: m->wiggle = fmaxf(m->wiggle, .16f * s); break;
    case KICK_TWIRL: m->twirl += side * 5.f * s; tess_let_go(m, 1.6f + .4f * s); break;  // (it spins for fun, not facing you)
    case KICK_NOD: m->push[TP_PITCH] += 2.6f * s; break;  // a little bow
    case KICK_FLINCH:  // away from the touch
        m->shift_rate[0] -= dx * 2.4f * s; m->shift_rate[1] -= dy * 2.4f * s;
        m->push[TP_PITCH] -= 1.6f * s;
        break;
    case KICK_NUZZLE:  // snuggling into it
        m->shift_rate[0] += dx * 1.8f * s; m->shift_rate[1] += dy * 1.8f * s;
        m->push[TP_YAW] += dx * 1.4f * s;
        break;
    case KICK_DIP:  // pressed: it gives way under the finger and tips away from it
        m->shift_rate[0] -= dx * 3.2f * s; m->shift_rate[1] += (2.4f - dy * 3.2f) * s;
        m->push[TP_PITCH] += dy * 1.6f * s;
        break;
    default: break;
    }
}

void tess_mood_event(face_t *f, face_event_t ev, float x) {
    tess_mood_t *m = &f->tess_mood;
    bool idle = f->mode == MODE_IDLE;
    tess_gaze_event(f, ev);
    switch (ev) {
    case FEV_TAP:
        m->perk = fmaxf(m->perk, .6f);
        if (idle) {  // the near side turns towards the touch
            start_move(f, MV_GLANCE);
            m->amp = clampf((x - 240) / 150, -1, 1) * .5f;
        }
        break;
    case FEV_PICKUP:
    case FEV_WAKE:
        m->perk = 1;
        if (idle) start_move(f, MV_GLANCE);
        break;
    case FEV_NOTIFY:  // an alert wiggle
        m->perk = 1;
        if (idle) { start_move(f, MV_WOBBLE); m->aux *= 1.5f; m->len = 2.2f; }
        break;
    case FEV_PET: m->perk = fmaxf(m->perk, .4f); break;
    default: break;
    }
}

// The offsets kicks leave: the carried shift bounces on an underdamped spring; twirl and wiggle
// die away; the barrel roll and the y-w roll run to their goals (whole turns, so they end where they began).
static void tess_spring_to(float *x, float *rate, float goal, float w, float dt) {
    *rate += (w * w * (goal - *x) - 2 * w * *rate) * dt;
    *x += *rate * dt;
}
static void carry_drag(tess_mood_t *m, float dt) {
    float drag_decay = expf(-dt * 1.6f);
    for (int k = 0; k < 2; k++) {
        m->drag_angle[k] += m->drag_speed[k] * dt;
        m->drag_speed[k] *= drag_decay;
        if (fabsf(m->drag_angle[k]) > 4 * PI) m->drag_angle[k] = fmodf(m->drag_angle[k], 4 * PI);
    }
}

static void carry_on(tess_mood_t *m, float dt) {
    static const float w = 11.f, zeta = .32f;
    for (int step = 0; step < 2; step++)
        for (int k = 0; k < 2; k++) {
            m->shift_rate[k] += (-w * w * m->shift[k] - 2 * zeta * w * m->shift_rate[k]) * (dt / 2);
            m->shift[k] += m->shift_rate[k] * (dt / 2);
        }
    for (int k = 0; k < TP_COUNT; k++) {
        float let_in = m->push[k] * fminf(1, dt * 5.f);
        m->rate[k] += let_in;
        m->push[k] -= let_in;
    }
    carry_drag(m, dt);
    m->twirl *= expf(-dt * 1.6f);
    m->twirl_now += (m->twirl - m->twirl_now) * fminf(1, dt * 5.f);  // a twirl picks up speed, it does not jerk
    m->wiggle *= expf(-dt * 2.4f);
    tess_spring_to(&m->barrel, &m->barrel_rate, m->barrel_goal, 6.f, dt);
    tess_spring_to(&m->yw, &m->yw_rate, m->yw_goal, 7.f, dt);
    if (m->barrel_goal > 4 * PI && m->barrel > 2 * PI) { m->barrel -= 2 * PI; m->barrel_goal -= 2 * PI; }  // a turn is a turn
    if (m->barrel_goal < -4 * PI && m->barrel < -2 * PI) { m->barrel += 2 * PI; m->barrel_goal += 2 * PI; }
    if (m->yw_goal > 4 * PI && m->yw > 2 * PI) { m->yw -= 2 * PI; m->yw_goal -= 2 * PI; }
    if (m->yw_goal < -4 * PI && m->yw < -2 * PI) { m->yw += 2 * PI; m->yw_goal += 2 * PI; }
}

static void idle_step(face_t *f, float dt, float sleepy, bool low, float to[TP_COUNT], float *spin) {
    tess_mood_t *m = &f->tess_mood;
    m->run += dt;
    m->next_4d -= dt;
    if (m->shaken && m->dizzy < .05f) {  // the giddiness is over: a long breath
        m->shaken = false;
        start_move(f, MV_REST);
        m->len += 2;
    } else if (m->run >= m->len) {
        start_move(f, pick_move(f, sleepy, low));
    }
    idle_targets(f, sleepy, low, to, spin);
    tess_play_targets(f, to, spin);
    if (f->rub.stage > 0) tess_rub_targets(f, to, spin);
}

void tess_mood_update(face_t *f, float dt) {
    static const float k_omega[TP_COUNT] = {3.f, 4.f, 6.f};  // spring stiffness, critically damped
    tess_mood_t *m = &f->tess_mood;
    float view = fabsf(f->view_q[1]) + fabsf(f->view_q[2]) + fabsf(f->view_q[3]);
    m->still = fabsf(view - m->view_sum) > .002f || f->tess_agitation > .1f ? 0.f : m->still + dt;
    m->view_sum = view;
    m->perk = fmaxf(0, m->perk - dt * .2f);
    m->dizzy = fmaxf(f->tess_agitation, m->dizzy - dt * .4f);
    if (m->dizzy > .5f) m->shaken = true;
    float sleepy = tess_quiet(fminf(f->idle_t, m->still), QUIET_SPAN);
    bool low = f->battery_low && !f->charging;
    float to[TP_COUNT] = {0}, spin = WAITING_SPIN;
    if (f->mode == MODE_IDLE) idle_step(f, dt, sleepy, low, to, &spin);
    else context_targets(f, to, &spin);
    tess_gaze_step(f, dt, to, &spin);
    m->spin += (spin * f->style.spin - m->spin) * fminf(1, dt * 1.4f);
    to[TP_ROLL] += m->wiggle * tess_sin(f->t * 10.5f);
    carry_on(m, dt);
    for (int k = 0; k < TP_COUNT; k++) {
        float omega = k_omega[k] * (k < TP_ROLL ? 1 + .4f * m->eager * (1 - smooth01(m->react / .55f)) : 1);  // eager: it follows faster (not while a reaction turns it)
        m->rate[k] += (omega * omega * (to[k] - m->pose[k]) - 2 * omega * m->rate[k]) * dt;
        m->pose[k] += m->rate[k] * dt;
    }
}
