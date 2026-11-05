#pragma once

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

// S3 1.8 V2, native portrait: Waveshare Gravitysphere maps screen x=-Ay,
// screen y=+Ax. Rotate both polar acceleration and axial angular velocity
// together before fusion; this quarter turn preserves the right-handed frame.
static inline void input_motion_to_screen(float vector[3]) {
#if CONFIG_IDF_TARGET_ESP32S3
    float x = vector[0];
    vector[0] = -vector[1];
    vector[1] = x;
#else
    (void)vector;  // C6 keeps its established sensor frame bit for bit.
#endif
}
