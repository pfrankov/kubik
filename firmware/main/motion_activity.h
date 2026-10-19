// Visible-screen activity, separate from the stricter lift gesture that may wake an auto-dark screen.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MOTION_ACTIVITY_MS 250
#define MOTION_ACTIVITY_TURN2 .0016f // about 2.3 degrees of accumulated gravity-direction change

typedef struct {
    float gravity[3];
    uint32_t last_ms;
    bool ready;
} motion_activity_t;

static inline bool motion_activity_step(motion_activity_t *s, const float gravity[3],
                                        float rotation, float dynamic, uint32_t now_ms) {
    float change = 0;
    for (int i = 0; i < 3; i++) {
        float d = gravity[i] - s->gravity[i];
        change += d * d;
    }
    bool moved = change >= MOTION_ACTIVITY_TURN2 || rotation >= .12f || dynamic >= .06f;
    if (s->ready && (!moved || (uint32_t)(now_ms - s->last_ms) < MOTION_ACTIVITY_MS)) return false;
    bool report = s->ready;
    for (int i = 0; i < 3; i++) s->gravity[i] = gravity[i];
    s->last_ms = now_ms;
    s->ready = true;
    return report;
}
