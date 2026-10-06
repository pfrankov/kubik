#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct { int taps; int64_t first_ms; bool unlocked; } menu_service_t;

// Five taps on the battery readout within four seconds reveal service controls (Reset and Events) for this menu visit.
static inline bool menu_service_tap(menu_service_t *s, int x, int y, int64_t now) {
    if (s->unlocked) return false;
    if (x < 355 || y > 55 || y < 0 || x > 480) {
        s->taps = 0;
        return false;
    }
    if (!s->taps || now - s->first_ms > 4000) {
        s->first_ms = now;
        s->taps = 0;
    }
    if (++s->taps < 5) return false;
    s->unlocked = true;
    return true;
}
