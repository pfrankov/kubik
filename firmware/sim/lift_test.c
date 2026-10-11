#include "../main/lift.h"
#include <assert.h>
#include <stdio.h>

#define COS35 0.82f
static lift_t l;
static int64_t now;

static void rest(int ms) {
    for (int t = 0; t < ms; t += 20, now += 20)
        assert(!lift_step(&l, &(lift_in_t){.dt_ms = 20, .still = true, .alignment = 1, .turned_cos = COS35, .now_ms = now, .last_shake_ms = -10000}));
}
// Moves for ms with the given peak acceleration and pose; true if a lift was reported.
static bool move(int ms, float dynamic, float alignment, int64_t last_shake) {
    bool lifted = false;
    for (int t = 0; t < ms; t += 20, now += 20)
        lifted |= lift_step(&l, &(lift_in_t){.dt_ms = 20, .dynamic = dynamic, .alignment = alignment, .turned_cos = COS35,
                                             .now_ms = now, .last_shake_ms = last_shake});
    return lifted;
}
static void fresh(void) { lift_init(&l); now = 100000; }

int main(void) {
    fresh(); rest(4000);
    assert(move(500, 0.15f, 0.6f, -10000));  // accelerated and turned: a lift
    assert(!move(500, 0.15f, 0.6f, -10000)); // reported once

    fresh(); rest(4000);
    assert(!move(1000, 0.5f, 0.99f, -10000)); // knock on the table: spikes, pose unchanged
    fresh(); rest(4000);
    assert(!move(2000, 0.02f, 0.6f, -10000)); // slow tilt of the surface: turned but no acceleration
    fresh(); rest(4000);
    assert(!move(500, 0.15f, 0.9f, -10000));  // nudge: under 35 degrees
    fresh(); rest(4000);
    assert(!move(500, 0.15f, 0.6f, now - 500)); // part of a shake
    fresh(); rest(1000);
    assert(!move(500, 0.15f, 0.6f, -10000));  // not at rest long enough: already being handled
    fresh(); rest(4000);
    assert(!move(3200, 0.04f, 0.99f, -10000)); // long carrying with no turn
    assert(!move(500, 0.15f, 0.6f, -10000));  // ...is not a fresh lift
    fresh(); rest(4000);
    assert(move(500, 0.15f, 0.6f, -10000)); rest(4000);
    assert(!move(500, 0.15f, 0.6f, -10000));  // set down and picked up inside the refractory time
    now += 3000; rest(4000);
    assert(move(500, 0.15f, 0.6f, -10000));
    puts("lift: lift, knock, slow tilt, nudge, shake, early move, carrying and repeat");
    return 0;
}
