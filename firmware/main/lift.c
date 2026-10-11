#include "lift.h"

void lift_init(lift_t *l) { *l = (lift_t){.last_lift_ms = -10000}; }

static void settle(lift_t *l, int dt_ms) {
    l->still_ms += dt_ms;
    l->moving_ms = l->turned_ms = 0;
    l->peak = 0;
    if (l->still_ms > LIFT_REST_MS) l->rested = true;
}

bool lift_step(lift_t *l, const lift_in_t *in) {
    if (in->still) { settle(l, in->dt_ms); return false; }
    l->still_ms = 0;
    if (!l->rested) return false;
    l->moving_ms += in->dt_ms;
    if (in->dynamic > l->peak) l->peak = in->dynamic;
    l->turned_ms = in->alignment < in->turned_cos ? l->turned_ms + in->dt_ms : 0;
    if (l->turned_ms >= LIFT_TURN_MS && l->peak >= LIFT_PEAK_G &&
        in->now_ms - l->last_lift_ms > LIFT_REFRACTORY_MS && in->now_ms - in->last_shake_ms > LIFT_SHAKE_GAP_MS) {
        l->last_lift_ms = in->now_ms;
        l->rested = false;
        return true;
    }
    if (l->moving_ms > LIFT_CARRY_MS) l->rested = false;
    return false;
}
