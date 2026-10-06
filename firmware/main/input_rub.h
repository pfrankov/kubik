// The touch task's finger strokes, handed to the face for petting (rub.h). Samples come at 50 Hz from the touch
// task; the app takes what accumulated once a frame.
#pragma once

#include <stdbool.h>

#include "rub.h"

// One touch sample: a finger is down at x, y (panel px), or it lifted.
void input_rub_sample(bool down, int x, int y);
// The stroke since the previous call (path and reversals are consumed; where the finger is stays).
void input_rub(rub_input_t *input);
// The "sim" "rub" test hook: the vigorous script of the simulator (sim_rub.c) at t seconds, on the belly or on the cloud.
void input_rub_script(float t, bool cloud, bool down);
