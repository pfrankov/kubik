// Schedule S3 preparation against the physical panel clock, two TE periods per frame.
#pragma once
#include <stdbool.h>

// Called after a frame. Returns true only if its next presentation deadline
// was already missed before this helper's deliberate blocking/yield.
// Dark waits remain notification-interruptible and reset the active phase.
bool frame_pacing_s3_wait(bool powered);
