#include "input_touch.h"
#include "input_hold.h"
#include "input_probe.h"
#include "input_rub.h"
#include "app.h"
#include <math.h>
#include <stdlib.h>

static void start_touch(input_touch_t *s, int x, int y, int64_t now_ms) {
    app_post(EV_TOUCH_DOWN, 1, 0);
    s->touching = true;
    s->first_drag = true;
    s->gesture_done = false;
    s->touch_x0 = s->touch_last_x = x;
    s->touch_y0 = s->touch_last_y = y;
    s->touch_path = 0;
    s->touch_started_ms = s->touch_activity_ms = now_ms;
}

static void update_touch_path(input_touch_t *s, int x, int y, int64_t now_ms) {
    if (!s->touching) {
        start_touch(s, x, y, now_ms);
        return;
    }
    s->touch_path += hypotf(x - s->touch_last_x, y - s->touch_last_y);
    s->touch_last_x = x;
    s->touch_last_y = y;
}

static void update_menu_drag(input_touch_t *s, int x, int y, bool menu) {
    if (menu && (s->first_drag || abs(x - s->drag_x) + abs(y - s->drag_y) >= 4)) {
        app_post(EV_DRAG, x | (s->first_drag ? 1 << 16 : 0), y);
        s->drag_x = x;
        s->drag_y = y;
        s->first_drag = false;
    }
}

static void update_touch_gesture(input_touch_t *s, int x, int y, int64_t now_ms, bool menu) {
    int dx = x - s->touch_x0, dy = y - s->touch_y0;
    float moved = hypotf(dx, dy);
    update_menu_drag(s, x, y, menu);
    if (input_hold_ready(menu, s->gesture_done, now_ms - s->touch_started_ms, s->touch_path)) {
        s->gesture_done = true;
        app_post(EV_MENU_HOLD, s->touch_x0, s->touch_y0);
    }
    if (!s->gesture_done && menu && -dy > 110 && abs(dx) < -dy && now_ms - s->touch_started_ms < 800) {
        s->gesture_done = true;
        app_post(EV_SWIPE_UP, 0, 0);
    }
    if (!menu && !s->gesture_done &&
        ((now_ms - s->touch_started_ms > 650 && moved < 30) || s->touch_path > 220)) {
        s->gesture_done = true;
        app_post(EV_PET, x, y);
    }
}

static void release_touch(input_touch_t *s, bool menu, int64_t now_ms) {
    s->touching = false;
    if (menu && !s->first_drag) app_post(EV_DRAG, s->drag_x | 1 << 17, s->drag_y);  // lets lists coast
    if (!s->gesture_done && now_ms - s->touch_started_ms < 450 && s->touch_path < 40) {
        app_post(EV_TAP, s->touch_x0, s->touch_y0);
    }
    app_post(EV_TOUCH_UP, 0, 0);
}

static void refresh_touch_activity(input_touch_t *s, int64_t now_ms) {
    if (now_ms - s->touch_activity_ms < 200) return;
    app_post(EV_TOUCH_DOWN, 0, 0);
    s->touch_activity_ms = now_ms;
}

static bool read_sample(input_touch_t *s, touch_frame_t *frame, bool *valid) {
    *valid = input_probe_read(frame);
    if (*valid) { s->missed_samples = 0; return true; }
    if (s->missed_samples < 3) s->missed_samples++;
    if (s->missed_samples < 3) return false;
    *frame = (touch_frame_t){0};
    return true;
}

static int update_contacts(input_touch_t *s, const touch_frame_t *frame, bool valid,
                           bool menu, int64_t now_ms) {
    unsigned count = touch_count(frame);
    int primary = touch_primary_step(&s->primary, frame);
    if (primary >= 0) {
        int x = frame->point[primary].x, y = frame->point[primary].y;
        update_touch_path(s, x, y, now_ms);
        if (count > 1) s->gesture_done = true;
        update_touch_gesture(s, x, y, now_ms, menu);
    } else if (s->touching) {
        if (!valid || count) s->gesture_done = true;
        release_touch(s, menu, now_ms);
    }
    if (count) refresh_touch_activity(s, now_ms);
    return primary;
}

bool input_touch_poll(input_touch_t *s, int64_t now_ms, bool menu, bool face_touch) {
    touch_frame_t frame;
    bool valid;
    if (!read_sample(s, &frame, &valid)) return false;
    bool touching = touch_count(&frame) != 0;
    int primary = update_contacts(s, &frame, valid, menu, now_ms);
    if (!face_touch) { frame = (touch_frame_t){0}; primary = -1; }
    input_rub_sample(&frame, primary);
    return touching;
}
