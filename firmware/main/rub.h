// Petting takes effort: how hard and how long a finger rubs a character builds an "affection" meter that fades
// when the rubbing stops. Portable C, shared by Plush, Tess, the touch task and the simulator's gesture tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { RUB_NONE = 0, RUB_NOTICE, RUB_GIGGLE, RUB_BLUSH, RUB_WIGGLE, RUB_STAGES } rub_stage_t;

// What the finger did since the last update: path length, direction reversals, where it is (panel px).
typedef struct { float path, x, y; int turns; bool down; } rub_input_t;
// Follows the touch samples (about 50 a second) and folds them into a rub_input_t.
typedef struct { float last_x, last_y, stroke_x, stroke_y; bool active; } rub_track_t;
// One touch sample (down: a finger is on the panel at x, y; else it lifted). Adds to `input` until the meter takes it.
void rub_touch(rub_track_t *track, rub_input_t *input, bool down, int x, int y);

typedef struct {
    float energy;         // 0..1 effort; the stage is read off it
    float speed, turns;   // smoothed finger speed (px/s) and reversals per second
    float hot;            // seconds spent at the top stage; joy needs a few
    float stage_t;        // seconds in this stage
    float idle_t;         // seconds since the finger last moved
    float cooldown;       // after joy, seconds before another can come
    float move_left;      // seconds until the reaction picks its next move
    uint8_t move, move_stage;  // the move (0..2) chosen for `move_stage`
    uint8_t stage;        // rub_stage_t of the energy, with a little hysteresis
    uint8_t peak;         // the highest stage since the effort last started
    uint16_t joys;        // times the top reward came
    bool rose, joy;       // this update: the stage went up; the reward came
} rub_t;

// Advances the meter by dt seconds. `area` is 0..1: how much of the finger's stroke lies on the ticklish part.
void rub_update(rub_t *rub, const rub_input_t *input, float dt, float area);
// Each stage has three moves, one picked at random for a while, never the one it just did.
bool rub_move_due(rub_t *rub, float dt);          // the stage changed or the move ran out: pick the next
void rub_pick_move(rub_t *rub, float random01);   // sets rub->move; the caller then sets rub->move_left
