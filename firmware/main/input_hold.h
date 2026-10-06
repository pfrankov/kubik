#pragma once
#include <stdbool.h>
#include <stdint.h>
// Initial hit eligibility is latched by the app; movement permanently disqualifies a hold.
static inline bool input_hold_ready(bool menu, bool completed, int64_t elapsed_ms, float path) {
    return menu && !completed && elapsed_ms >= 800 && path < 16;
}
