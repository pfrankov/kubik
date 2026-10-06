#include "tess_internal.h"

// Rubbing Tess (the effort meter is rub.c). There is no belly to rub and no face to squint, so effort shows
// as motion and colour: it perks up, spins faster, rolls over in 4D and, after a few seconds of vigorous
// rubbing, rewards you with the heart. Every stage has three moves and picks one at random, never the one it
// just did. All of them are turns of the rigid body (yaw, pitch, roll, the y-w plane), and the palette warms
// from cyan towards gold as the effort grows.
enum { PERK_GLANCE, PERK_LOOK_UP, PERK_HALF_TURN };
enum { SPIN_STEADY, SPIN_WIGGLY, SPIN_TO_AND_FRO };
enum { ROLL_4D, ROLL_BARREL, ROLL_CORKSCREW };
enum { EXCITED_BOTH, EXCITED_SWING, EXCITED_DOUBLE };

static const float k_move_s[RUB_STAGES][2] = {{0, 0}, {1.6f, 2.4f}, {1.4f, 2.2f}, {1.5f, 2.4f}, {1.f, 1.7f}};

static void perk_up(face_t *f, int pick, float side) {
    tess_mood_t *m = &f->tess_mood;
    m->rub_hold[2] = m->spin;
    if (pick == PERK_GLANCE) m->rub_hold[0] = clampf((f->rub_in.x - 240) / 150, -1, 1) * .5f;
    if (pick == PERK_LOOK_UP) { m->rub_hold[1] = -.3f; tess_mood_kick(f, KICK_HOP, .5f, 240, 255); }
    if (pick == PERK_HALF_TURN) m->twirl += side * 3.f;
}

static void quicken(tess_mood_t *m, int pick, float side) {  // a quickening spin: three ways to have it
    if (pick == SPIN_WIGGLY) m->wiggle = fmaxf(m->wiggle, .14f);
    if (pick == SPIN_TO_AND_FRO) m->twirl += side * 2.4f;
    m->rub_hold[2] *= pick == SPIN_STEADY ? 1.f : 1.3f;
}

static void roll_over(tess_mood_t *m, int pick, float side) {  // distinct from the endless x-w and z-w turn of waiting
    if (pick == ROLL_4D) m->yw_goal += side * PI / 2;
    if (pick == ROLL_BARREL) m->barrel_goal += side * 2 * PI;
    if (pick == ROLL_CORKSCREW) { m->twirl += side * 5.f; m->yw_goal += side * PI / 2; }
    m->rub_hold[2] *= .6f;
}

static void get_excited(tess_mood_t *m, int pick, float side) {  // bigger, faster and more at once
    if (pick == EXCITED_BOTH) { m->yw_goal += side * PI; m->barrel_goal += side * 2 * PI; }
    if (pick == EXCITED_SWING) m->rub_hold[2] = side * 1.9f;
    if (pick == EXCITED_DOUBLE) { m->yw_goal += side * PI / 2; m->twirl += side * 4.f; }
}

static void start_move(face_t *f, int stage, int pick, float side) {
    tess_mood_t *m = &f->tess_mood;
    m->rub_hold[0] = m->rub_hold[1] = 0;
    m->rub_hold[2] = side * (.4f + 1.4f * f->rub.energy);
    switch (stage) {
    case RUB_NOTICE: perk_up(f, pick, side); break;
    case RUB_GIGGLE: quicken(m, pick, side); break;
    case RUB_BLUSH: roll_over(m, pick, side); break;
    default: get_excited(m, pick, side); break;
    }
}

void tess_rub_step(face_t *f, float dt) {
    tess_mood_t *m = &f->tess_mood;
    rub_t *r = &f->rub;
    bool idle = f->mode == MODE_IDLE;
    float goal = idle ? fmaxf(r->energy, f->tess_play.glow) : 0.f;
    m->warm += (goal - m->warm) * fminf(1, dt * (goal > m->warm ? 3.f : 1.2f));
    if (r->joy && idle) {  // the reward: the heart, and a happy twirl
        face_set_emotion(f, EMO_LOVE, 3.5f);
        m->twirl += 4.f;
    }
    if (!idle || !rub_move_due(r, dt)) return;
    rub_pick_move(r, frand(f));
    start_move(f, r->stage, r->move, frand(f) < .5f ? -1.f : 1.f);
    r->move_left = frange(f, k_move_s[r->stage][0], k_move_s[r->stage][1]);
    f->tess_energy = 1;  // the glow follows the fun
}

// What the current move holds: where the camera looks and how fast it circles.
void tess_rub_targets(face_t *f, float to[TP_COUNT], float *spin) {
    const tess_mood_t *m = &f->tess_mood;
    to[TP_YAW] = m->rub_hold[0];
    to[TP_PITCH] = m->rub_hold[1];
    *spin = m->rub_hold[2];
    if (f->rub.stage == RUB_WIGGLE && f->rub.move == EXCITED_SWING) to[TP_PITCH] = .4f * tess_sin(f->t * 7.5f);
}
