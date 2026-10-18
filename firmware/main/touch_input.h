// A complete panel sample. Slots follow stable contacts, never report order.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TOUCH_POINTS 2
typedef struct { int16_t x, y; bool down; uint8_t id; } touch_point_t;
typedef struct { touch_point_t point[TOUCH_POINTS]; } touch_frame_t;
typedef struct { uint8_t slot, id; bool active, blocked; } touch_primary_t;
typedef struct { uint8_t id[TOUCH_POINTS]; bool used[TOUCH_POINTS]; } touch_tracker_t;
typedef struct { uint16_t x, y; uint8_t id; } touch_contact_t;

// A report contains up to two points, but its 4-bit IDs need not be 0 or 1.
// Retained contacts keep their slot even if the report order changes.
static inline void touch_tracker_update(touch_tracker_t *tracker, const touch_contact_t *points,
                                        unsigned count, touch_frame_t *frame) {
    *frame = (touch_frame_t){0};
    if (count > TOUCH_POINTS) { *tracker = (touch_tracker_t){0}; return; }
    bool taken[TOUCH_POINTS] = {0};
    for (unsigned slot = 0; slot < TOUCH_POINTS; slot++) {
        if (!tracker->used[slot]) continue;
        tracker->used[slot] = false;
        for (unsigned i = 0; i < count; i++) {
            if (points[i].id != tracker->id[slot]) continue;
            frame->point[slot] = (touch_point_t){points[i].x, points[i].y, true, points[i].id};
            tracker->used[slot] = taken[i] = true;
        }
    }
    for (unsigned i = 0; i < count; i++) {
        if (taken[i]) continue;
        for (unsigned slot = 0; slot < TOUCH_POINTS; slot++) {
            if (tracker->used[slot]) continue;
            tracker->used[slot] = true; tracker->id[slot] = points[i].id;
            frame->point[slot] = (touch_point_t){points[i].x, points[i].y, true, points[i].id};
            break;
        }
    }
}

static inline unsigned touch_count(const touch_frame_t *frame) {
    unsigned count = 0;
    for (unsigned i = 0; i < TOUCH_POINTS; i++) count += frame->point[i].down;
    return count;
}

// Ordinary gestures keep their original finger. After it lifts, remaining fingers
// cannot start a new drag/tap until the glass is completely released.
static inline int touch_primary_step(touch_primary_t *primary, const touch_frame_t *frame) {
    if (!touch_count(frame)) { *primary = (touch_primary_t){0}; return -1; }
    const touch_point_t *current = &frame->point[primary->slot];
    if (primary->active && current->down && current->id == primary->id) return primary->slot;
    if (primary->active) { primary->active = false; primary->blocked = true; }
    if (primary->blocked) return -1;
    for (unsigned i = 0; i < TOUCH_POINTS; i++) {
        if (!frame->point[i].down) continue;
        primary->slot = i; primary->id = frame->point[i].id; primary->active = true;
        return i;
    }
    return -1;
}
