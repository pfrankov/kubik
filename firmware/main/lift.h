// Tells a deliberate lift from everyday movement, from IMU summaries only (no hardware).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LIFT_REST_MS 3000      // quiet this long before a lift can be recognised
#define LIFT_TURN_MS 260       // the pose must stay changed this long
#define LIFT_PEAK_G 0.06f      // a hand accelerates the body at least this much; a slow table tilt does not
#define LIFT_CARRY_MS 3000     // movement this long is being carried, not a fresh lift
#define LIFT_REFRACTORY_MS 6000
#define LIFT_SHAKE_GAP_MS 3000 // shaking is its own gesture

typedef struct {
    int still_ms, moving_ms, turned_ms;
    bool rested;
    float peak;
    int64_t last_lift_ms;
} lift_t;

typedef struct {
    int dt_ms;
    bool still;
    float dynamic;       // linear acceleration, g
    float alignment;     // cos of the angle between gravity and the resting pose
    float turned_cos;    // alignment at or below this counts as turned
    int64_t now_ms, last_shake_ms;
} lift_in_t;

void lift_init(lift_t *l);
// True once per lift.
bool lift_step(lift_t *l, const lift_in_t *in);
