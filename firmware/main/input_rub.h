// The touch task's finger strokes, handed to the face for petting (rub.h). Samples come at 50 Hz from the touch
// task; the app takes what accumulated once a frame.
#pragma once

#include <stdbool.h>

#include "rub.h"
#include "touch_input.h"

// Complete touch sample and the primary finger that owns ordinary gestures (-1: released).
void input_rub_sample(const touch_frame_t *frame, int primary);
// The stroke since the previous call (path and reversals are consumed; where the finger is stays).
void input_rub(rub_input_t *input, touch_frame_t *frame);
// The "sim" "rub" test hook: the vigorous script of the simulator (sim_rub.c) at t seconds, on the belly or on the cloud.
void input_rub_script(float t, bool cloud, bool down);
