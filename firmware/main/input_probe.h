#pragma once
#include <stdbool.h>
#include "touch_input.h"

// USB-only diagnostics: one controller sample, expiring after 350 ms.
bool input_probe_sample(bool down, int x, int y);
bool input_probe_read(touch_frame_t *frame);
