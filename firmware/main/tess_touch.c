#include "tess_internal.h"

// A finger on Tess. It reacts the frame the finger lands, before the app has decided what the touch is (a tap
// comes only on release): it gives way under the press, and its face turns towards the finger on a spring that
// overshoots a little and settles. Moving, the finger takes hold of the front and turns it, one to one; letting
// go, momentum carries on in both axes; horizontal games can swing back, and three swipes in a run make it giddy. Held or
// stroked, it purrs (a faint tremble) and afterwards settles into a contented sway. Landing on it after it
// asked for attention, or after it dodged, delights it. The body only turns and is carried; what runs through the
// points (the wave of a press) is an effect on them, as the trembling of shaken points is (tess_motion.c).
#define LOOK_YAW .3f          // rad of camera yaw / pitch towards a finger at the edge of the cloud
#define LOOK_PITCH .2f
#define LOOK_OMEGA 24.f       // 1/s, and a damping under 1: it overshoots the finger a little
#define LOOK_ZETA .5f
#define SWAY_YAW .12f         // the contented sway (rad)
#define SWAY_PITCH .04f
#define LINGER_S .8f          // s the face stays turned to where the finger was
#define GLOW .3f              // palette warmth the contentment leaves
#define GRAB_PER_PX (1.f / 150.f)  // rad the front turns per px of drag
#define CATCH_RATE 9.f        // 1/s at which a finger on it takes the spin out of it
#define GRAB_DEADBAND 1.5f    // px a frame below which it is sensor noise
#define FLING_PX_S 380.f      // px/s a finger must leave at to fling it
#define FLING_MAX 9.f         // rad/s
#define FLING_RUN_S 3.f       // swipes this close together are a run
#define SWING_BACK_S 1.1f
#define PET_HOLD_S .65f       // as the app's: a held or long stroke is petting
#define PET_PATH 220.f
#define PURR_RAD .045f
#define DODGE_ALONE_S 12.f    // it plays hard to get only when it has been left alone a while
#define DODGE_ODDS .2f
#define DODGE_JUMP 7.f        // units/s of sideways speed it leaps away with (about 35 px)
#define DODGE_WINDOW_S 1.6f
#define DODGE_COOL_S 25.f
#define RIPPLE_SPEED 6.5f     // units/s the front of a press's wave travels (it crosses the cube in about .4 s)
#define RIPPLE_LIFE_S .9f
#define RIPPLE_HALF .5f       // units, half the width of the front
#define RIPPLE_PUSH .3f       // units the front pushes a point away from the press, and (below) towards the viewer
#define RIPPLE_LIFT .35f
#define RIPPLE_FADE .5f       // 1/units: the wave weakens with distance

// A press sets a wave going: a front that runs out through the points, pushing each away from the finger and a little
// towards the viewer as it passes. The points follow their targets on springs, so they overshoot and settle.
// The oldest of a few waves makes way for a new one.
void tess_touch_wave(face_t *f, float x, float y) {
    tess_ripple_t *slot = f->tess_ripple;
    for (int k = 1; k < TESS_RIPPLES; k++) if (f->tess_ripple[k].age > slot->age) slot = &f->tess_ripple[k];
    *slot = (tess_ripple_t){(x - 240) / TESS_PX_PER_UNIT, (y - 255) / TESS_PX_PER_UNIT, 0};
}

bool tess_ripple_active(const face_t *f) {
    for (int k = 0; k < TESS_RIPPLES; k++) if (f->tess_ripple[k].age < RIPPLE_LIFE_S) return true;
    return false;
}

void tess_ripple_point(const face_t *f, float *x, float *y, float *z) {
    for (int k = 0; k < TESS_RIPPLES; k++) {
        const tess_ripple_t *r = &f->tess_ripple[k];
        if (r->age >= RIPPLE_LIFE_S) continue;
        float dx = *x - r->x, dy = *y - r->y, d2 = dx * dx + dy * dy, ring = r->age * RIPPLE_SPEED;
        float near = fmaxf(0, ring - RIPPLE_HALF), far = ring + RIPPLE_HALF;
        if (d2 < near * near || d2 > far * far) continue;  // (not yet reached, or past)
        float d = sqrtf(d2) + 1e-3f, u = (d - ring) / RIPPLE_HALF;
        float gain = (1 - u * u) * (1 - r->age / RIPPLE_LIFE_S) / (1 + RIPPLE_FADE * d);
        *x += dx / d * RIPPLE_PUSH * gain;
        *y += dy / d * RIPPLE_PUSH * gain;
        *z += RIPPLE_LIFT * gain;
    }
}

// A finger landed on (x, y).
static void land(face_t *f, float x, float y) {
    tess_play_t *p = &f->tess_play;
    tess_feel_event(f, ME_TOUCH, .5f, (x - 240) / 240);
    p->down = true;
    p->down_t = p->stroke = p->up_t = 0;
    p->petted = false;
    p->finger[0] = x; p->finger[1] = y;
    p->speed[0] = p->speed[1] = 0;
    f->tess_touch_x = x; f->tess_touch_y = y; f->tess_touch_t = 0;
    f->tess_energy = 1;
    bool neglected = p->alone > DODGE_ALONE_S;
    bool glad = tess_play_touched(f);
    bool idle = f->mode == MODE_IDLE;
    if (idle) tess_gaze_notice(f, 6.f);
    if (!glad && idle && f->rub.stage == 0 && neglected && p->dodge_cool <= 0 && frand(f) < DODGE_ODDS * f->style.dodge) {  // "not so fast"
        p->dodge_t = DODGE_WINDOW_S;
        p->dodge_cool = DODGE_COOL_S;
        f->tess_mood.shift_rate[0] += (x < 240 ? 1.f : -1.f) * DODGE_JUMP;  // away from the finger
        tess_kick(f, KICK_HOP, .6f);
        tess_after(f, .5f, AFTER_PEEK, 0);
        tess_cue(f, TC_DODGE, 1.f, (x - 240) / 240);
        return;
    }
    tess_touch_wave(f, x, y);
    tess_kick(f, KICK_DIP, 1.f);
    tess_cue(f, TC_TOUCH, 1.f - clampf(hypotf(x - 240, y - 255) / 240, 0, 1), (x - 240) / 240);
}

// A game yields the complete gesture to ordinary touch, including its origin.
void tess_touch_resume(face_t *f, float x, float y, float age) {
    tess_touch_wave(f, x, y);
    tess_feel_event(f, ME_TOUCH, .5f, (x - 240) / 240);
    tess_cue(f, TC_TOUCH, .12f, (x - 240) / 240);
    f->tess_touch_x = x; f->tess_touch_y = y; f->tess_touch_t = 0;
    f->tess_energy = 1;
    tess_play_t *p = &f->tess_play;
    p->down = true;
    p->down_t = age;
    p->stroke = 0;
    p->finger[0] = x; p->finger[1] = y;
    p->speed[0] = p->speed[1] = 0;
    p->petted = age >= PET_HOLD_S;
}

// The finger is at (x, y): how far and how fast it moved, whether that is petting, and what it does to the cube.
static void drag(face_t *f, float dt, float x, float y) {
    tess_play_t *p = &f->tess_play;
    tess_mood_t *m = &f->tess_mood;
    float d[2] = {x - p->finger[0], y - p->finger[1]};
    p->finger[0] = x; p->finger[1] = y;
    p->stroke += hypotf(d[0], d[1]);
    p->down_t += dt;
    for (int k = 0; k < 2; k++) p->speed[k] += (d[k] / dt - p->speed[k]) * fminf(1, dt * 14.f);
    if (p->stroke > PET_PATH || p->down_t > PET_HOLD_S) p->petted = true;
    if (f->mode != MODE_IDLE) return;
    float held_decay = expf(-dt * CATCH_RATE);
    m->twirl *= held_decay;  // a hand on it stops the spin
    m->twirl_now *= held_decay;
    for (int k = 0; k < 2; k++) m->drag_speed[k] *= held_decay;
    if (hypotf(d[0], d[1]) > GRAB_DEADBAND && f->rub.stage == 0) {
        for (int k = 0; k < 2; k++) m->drag_angle[k] += d[k] * GRAB_PER_PX;
        tess_let_go(m, .4f);
    }
    if (p->petted && hypotf(p->speed[0], p->speed[1]) < 120.f) m->wiggle = fmaxf(m->wiggle, PURR_RAD);  // purring
}

// Release carries the two-axis turn; horizontal gestures retain their swing-back game.
static void fling(face_t *f) {
    tess_play_t *p = &f->tess_play;
    tess_mood_t *m = &f->tess_mood;
    float speed = hypotf(p->speed[0], p->speed[1]);
    float w = fminf(speed * GRAB_PER_PX, FLING_MAX), force = w / FLING_MAX;
    int axis = fabsf(p->speed[0]) >= fabsf(p->speed[1]) ? 0 : 1;
    float side = p->speed[axis] < 0 ? -1.f : 1.f;
    for (int k = 0; k < 2; k++) m->drag_speed[k] = w * p->speed[k] / speed;
    tess_let_go(m, 2.f);
    p->flings = p->run_t > 0 ? p->flings + 1 : 1;
    p->run_t = FLING_RUN_S;
    tess_cue(f, TC_FLING, force, side);
    if (p->flings >= 3) {
        p->flings = 0;
        m->barrel_goal += side * 2 * PI;
        tess_play_glad(f);
    } else if (axis == 0) {
        tess_after(f, SWING_BACK_S, AFTER_SWING, -side * (.4f + .6f * force));
    }
}

static void lift(face_t *f) {
    tess_play_t *p = &f->tess_play;
    bool idle = f->mode == MODE_IDLE && f->rub.stage == 0;
    p->down = false;
    p->up_t = 0;
    if (idle && hypotf(p->speed[0], p->speed[1]) > FLING_PX_S) fling(f);
    else if (idle && p->petted) tess_after(f, .3f, AFTER_CONTENT, 0);
    p->speed[0] = p->speed[1] = 0;
}

// Where its face is turned: to the finger while it is down, else the slow sway of contentment, else home.
static void steer_look(face_t *f, float dt) {
    tess_play_t *p = &f->tess_play;
    float to[2] = {0, 0}, k = smooth01(p->content_t * 1.5f / TESS_CONTENT_S) * smooth01((TESS_CONTENT_S - p->content_t) / .8f);  // (it settles into the sway and out of it)
    if (!p->down) p->up_t += dt;
    float at = p->down ? 1.f : smooth01(1.f - p->up_t / LINGER_S);  // (it lingers where the finger was)
    if (f->mode == MODE_IDLE && at > 0) {
        to[0] = clampf((p->finger[0] - 240) / 150, -1, 1) * LOOK_YAW * at;
        to[1] = -clampf((p->finger[1] - 255) / 150, -1, 1) * LOOK_PITCH * at;  // (a finger below tips the face down)
    } else if (f->mode == MODE_IDLE) {
        to[0] = SWAY_YAW * k * tess_sin(f->t * 2.f);
        to[1] = SWAY_PITCH * k * tess_sin(f->t * 2.f + 1.2f);
    }
    for (int i = 0; i < 2; i++) {
        p->look_rate[i] += (LOOK_OMEGA * LOOK_OMEGA * (to[i] - p->look[i]) - 2 * LOOK_ZETA * LOOK_OMEGA * p->look_rate[i]) * dt;
        p->look[i] += p->look_rate[i] * dt;
    }
    p->glow = f->mode == MODE_IDLE ? GLOW * k : 0.f;
}

#define JOY_AFTER_STAGE_S .5f  // s from a high rub stage to the gladness that follows it

void tess_touch_waves_step(face_t *f, float dt) {
    for (int k = 0; k < TESS_RIPPLES; k++) f->tess_ripple[k].age = fminf(99.f, f->tess_ripple[k].age + dt);
}

void tess_touch_update(face_t *f, float dt) {
    tess_play_t *p = &f->tess_play;
    const rub_input_t *in = &f->rub_in;
    if (dt > 0) {
        if (in->down && !p->down) land(f, in->x, in->y);
        else if (in->down) drag(f, dt, in->x, in->y);
        else if (p->down) lift(f);
    }
    tess_touch_waves_step(f, dt);
    if (f->rub.rose) {  // (the reward, the heart, has its own sound: tess_emotion)
        tess_cue(f, TC_RUB, f->rub.stage / 4.f, 0);
        if (f->rub.stage >= RUB_BLUSH) tess_after(f, JOY_AFTER_STAGE_S, AFTER_JOY_CUE, 0);
    }
    steer_look(f, dt);
}
