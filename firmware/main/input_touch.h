// Touch sampling/gesture ownership, separate from motion and physical buttons.
#pragma once
#include "touch_input.h"

typedef struct {
    bool touching, gesture_done, first_drag;
    int drag_x, drag_y, touch_x0, touch_y0, touch_last_x, touch_last_y;
    float touch_path;
    int64_t touch_started_ms, touch_activity_ms;
    uint8_t missed_samples;
    touch_primary_t primary;
} input_touch_t;

// True if the accepted controller sample has any fingers on the glass.
// Primary gestures remain single-finger; the face receives the complete sample.
bool input_touch_poll(input_touch_t *state, int64_t now_ms, bool menu, bool face_touch);
