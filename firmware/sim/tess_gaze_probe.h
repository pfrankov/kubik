// Host helper of the gaze test and the simulator's mood log: how directly Tess's face looks at the viewer. Header only.
#pragma once
#include <math.h>
#include "../main/tess_internal.h"

// Its face's normal as the viewer sees it: out[0] to the screen's right, out[2] to the viewer; size is the normal's length
// (under .3 it is mostly in the 4th dimension and there is none to speak of).
static inline float tess_gaze_normal(const face_t *f, float out[3]) {
    tess_turn4d_t turn;
    tess_turn4d_prepare(f, &turn);
    float n[4] = {0, 0, 0, 0}, camera[6], view[3][3];
    n[f->tess_mood.cell >> 1] = f->tess_mood.cell & 1 ? -1.f : 1.f;
    tess_turn4d_apply(&turn, n);
    float size = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (size < .3f) return 0;
    tess_camera_turns(f, camera);
    tess_camera_apply(camera, n);
    tess_view_matrix(f, view);
    tess_transform_point(view, n, out, false);
    return size;
}

// Cos of the angle between its face's normal and the way to the viewer (-1: not facing at all).
static inline float tess_gaze_facing(const face_t *f) {
    float out[3], size = tess_gaze_normal(f, out);
    return size > 0 ? out[2] / size : -1;
}

// How far its face is turned about the vertical from the viewer, rad: positive to the screen's right (0 when it has none).
static inline float tess_gaze_turn(const face_t *f) {
    float out[3];
    return tess_gaze_normal(f, out) > 0 ? atan2f(out[0], out[2]) : 0;
}
