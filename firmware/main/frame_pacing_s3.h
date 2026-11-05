// Schedule S3 preparation against the physical panel clock, two TE periods per frame.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

// Called after a frame. Reports a skipped expected presentation edge or a
// deadline already overdue before this helper's deliberate blocking/yield.
// Dark waits remain notification-interruptible and reset the active phase.
bool frame_pacing_s3_wait(bool powered);
// After all preparation, accept the target presentation edge, never an earlier
// slot. At most two fresh TE waits (40 ms each); propagate missing-clock errors.
esp_err_t frame_pacing_s3_sync(void);
