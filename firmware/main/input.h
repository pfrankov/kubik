// Buttons, touch gestures and motion (QMI8658), turned into app events.
#pragma once

#include <stdbool.h>

void input_init(void);
// Latest tilt, -1..1 (gravity relative to the resting pose).
void input_tilt(float *x, float *y);
// Gyro-assisted viewpoint, adaptive neutral after sustained rest.
void input_view_tilt(float *x, float *y);
// The same as an orientation (w, x, y, z), predicted over the display latency.
void input_view_quat(float q[4]);
void input_view_active(bool active);
// Velocity change from jolts (gravity removed) since the previous call, in g*s, along the
// screen's x (right) and y (down). Moves the face around inside its glass.
void input_jolt(float *dvx, float *dvy);
// Gravity's pull along the screen relative to the resting pose, in g (x right, y down):
// tilt Kubik and the face slides towards the lower side of its glass.
void input_slide(float *x, float *y);
// Gravity along the screen, absolute (not relative to the resting pose), in g (x right, y down).
void input_gravity(float *x, float *y);
// Settings menu open: touches are reported as EV_DRAG (for its sliders) and never as petting.
void input_set_menu(bool on);
// Screen dark: the input task sleeps between key and touch interrupts and samples at half rate.
void input_set_dark(bool dark);
