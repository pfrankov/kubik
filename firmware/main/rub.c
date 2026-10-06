#include "rub.h"

#include <math.h>

#define TURN_PX 14.f       // a stroke must run this far before turning back counts as a reversal
#define ENERGY_GAIN .5f    // effort per second at a full rub: speed 700 px/s, 3.5 reversals/s
#define DECAY_HELD .1f     // per second while the finger stays down without moving
#define DECAY_FREE .34f    // per second after it lifts
#define JOY_HOT_S 1.3f     // seconds at the top stage before joy
#define JOY_COOLDOWN_S 6.f
static const float k_stage_at[RUB_STAGES] = {0, .17f, .36f, .58f, .8f};

void rub_touch(rub_track_t *t, rub_input_t *in, bool down, int x, int y) {
    in->down = down;
    if (!down) { t->active = false; return; }
    in->x = (float)x; in->y = (float)y;
    if (!t->active) {  // a new stroke starts here
        t->active = true;
        t->last_x = (float)x; t->last_y = (float)y;
        t->stroke_x = t->stroke_y = 0;
        return;
    }
    float dx = x - t->last_x, dy = y - t->last_y, d = hypotf(dx, dy);
    t->last_x = (float)x; t->last_y = (float)y;
    if (d < 1.f) return;
    in->path += d;
    float stroke = hypotf(t->stroke_x, t->stroke_y);
    if (stroke > TURN_PX && t->stroke_x * dx + t->stroke_y * dy < -.25f * stroke * d) {
        in->turns++;
        t->stroke_x = dx; t->stroke_y = dy;
    } else {
        t->stroke_x += dx; t->stroke_y += dy;
    }
}

static float ease(float value, float target, float rate, float dt) { return value + (target - value) * fminf(1.f, rate * dt); }

static uint8_t stage_of(const rub_t *r) {
    int stage = r->stage;
    while (stage < RUB_STAGES - 1 && r->energy >= k_stage_at[stage + 1]) stage++;
    while (stage > 0 && r->energy < k_stage_at[stage] - .04f) stage--;
    return (uint8_t)stage;
}

// Effort in: the vigour of the rub (fast, with reversals) over the ticklish spot, less the decay.
static void gain_effort(rub_t *r, const rub_input_t *in, float dt, float area) {
    bool moving = in->down && in->path > 0.f;
    r->speed = ease(r->speed, in->down ? in->path / dt : 0.f, 8.f, dt);
    r->turns = ease(r->turns, in->turns / dt, 3.f, dt);
    r->idle_t = moving ? 0.f : r->idle_t + dt;
    float vigour = fminf(r->speed / 700.f, 1.5f) * (.3f + .7f * fminf(r->turns / 3.5f, 1.f));
    if (moving) r->energy += vigour * area * ENERGY_GAIN * dt;
    r->energy -= dt * (in->down && r->idle_t < .25f ? DECAY_HELD : DECAY_FREE);
    r->energy = r->energy < 0 ? 0 : r->energy > 1 ? 1 : r->energy;
}

void rub_update(rub_t *r, const rub_input_t *in, float dt, float area) {
    r->rose = r->joy = false;
    if (dt <= 0) return;
    gain_effort(r, in, dt, area);
    uint8_t stage = stage_of(r);
    r->rose = stage > r->stage;
    r->stage_t = stage == r->stage ? r->stage_t + dt : 0.f;
    r->stage = stage;
    if (stage > r->peak) r->peak = stage;
    if (stage == 0 && r->energy < .02f) r->peak = 0;
    r->hot = stage == RUB_STAGES - 1 ? r->hot + dt : fmaxf(0.f, r->hot - dt);
    r->cooldown = fmaxf(0.f, r->cooldown - dt);
    if (r->hot >= JOY_HOT_S && r->cooldown <= 0) {  // sustained, vigorous: the reward
        r->joy = true;
        r->joys++;
        r->hot = 0;
        r->cooldown = JOY_COOLDOWN_S;
        r->energy = .5f;
    }
}

bool rub_move_due(rub_t *r, float dt) {
    if (r->stage == 0) { r->move_stage = 0; return false; }
    r->move_left -= dt;
    return r->move_left <= 0 || r->move_stage != r->stage;
}

void rub_pick_move(rub_t *r, float random01) {
    int pick = (int)(random01 * 3) % 3;
    if (r->move_stage == r->stage && pick == r->move) pick = (pick + 1 + (int)(random01 * 37) % 2) % 3;
    r->move = (uint8_t)pick;
    r->move_stage = r->stage;
}
