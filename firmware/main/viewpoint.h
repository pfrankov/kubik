#pragma once
#include <stdbool.h>
// Device orientation relative to its resting pose (q: w, x, y, z; body axes: x, y along the screen, z into it).
// p is q carried forward by the display latency, so the drawn view keeps up with the hand.
typedef struct { float q[4], p[4], quiet, x, y; } viewpoint_t;
void viewpoint_init(viewpoint_t *v);
// Bias-corrected body-axis gyro, radians/sec. Rebase only during sustained rest.
void viewpoint_step(viewpoint_t *v, const float gyro[3], bool still, float dt);

