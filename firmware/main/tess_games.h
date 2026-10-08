// Local discoveries. The app restores durable progress before the first frame;
// render-only fixtures remain a fully formed Tess until they opt into this model.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "tess_progress.h"

typedef enum { TESS_GAME_NONE, TESS_GAME_ECHO, TESS_GAME_CATCH } tess_game_t;
typedef enum { TESS_GAME_REST, TESS_GAME_SHOW, TESS_GAME_WAIT, TESS_GAME_CELEBRATE } tess_game_phase_t;
typedef struct {
    bool ready, available;
    uint32_t round;              // keeps deferred input out of a replacement round
    uint8_t progress, game, phase, tier, step, taps, pending;
    float age, clock, deadline, last_tap, tap_x, tap_y;
    bool finger_down, circle_valid;
    float finger_age, finger_x, finger_y, circle_angle, circle_sweep;
    float form, fold, pulse, offset[2];
    float hit[2];                // rendered cloud center, panel pixels
    float last_hit[2];
    bool hit_valid;
    uint8_t trick, next_trick;
    float trick_age, trick_wait;
} tess_games_t;
