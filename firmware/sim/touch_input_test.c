#include "touch_report.h"
#include "input_touch.h"
#include "input_rub.h"
#include "app.h"
#include "tess_sky_play.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static touch_frame_t next;
static bool valid = true;
static unsigned events[64];
static int drag_x, drag_y;
bool input_probe_read(touch_frame_t *frame) { *frame = next; return valid; }
void app_post(app_ev_type_t type, int a, int b) {
    assert(type >= 0 && type < 64); events[type]++;
    if (type == EV_DRAG) { drag_x = a & 0xffff; drag_y = b; }
}

static void point(uint8_t *data, unsigned index, unsigned id, unsigned state, unsigned x, unsigned y) {
    uint8_t *p = data + index * 5 + (index ? 2 : 0);
    p[0] = id << 4 | state; p[1] = x >> 4; p[2] = y >> 4;
    p[3] = (x & 15) << 4 | (y & 15);
}
static void decoder_and_tracker(void) {
    uint8_t data[CST9217_REPORT_BYTES] = {0};
    touch_report_t report;
    touch_tracker_t tracker = {0};
    touch_frame_t frame;
    data[6] = 0xab;
    assert(touch_report_decode(data, sizeof data, &report)); assert(!report.count);
    data[5] = 1; point(data, 0, 14, 6, 479, 0);
    assert(touch_report_decode(data, sizeof data, &report)); assert(report.count == 1);
    assert(report.point[0].id == 14); assert(report.point[0].x == 479); assert(report.point[0].y == 0);
    data[5] = 2;
    point(data, 0, 15, 6, 21, 31); point(data, 1, 1, 6, 420, 390);
    assert(touch_report_decode(data, sizeof data, &report)); assert(report.count == 2);
    assert(report.point[0].id == 15); assert(report.point[1].id == 1);
    touch_tracker_update(&tracker, report.point, report.count, &frame);
    assert(frame.point[0].id == 15); assert(frame.point[1].id == 1);
    point(data, 0, 1, 6, 410, 380); point(data, 1, 15, 6, 22, 32);
    assert(touch_report_decode(data, sizeof data, &report));
    touch_tracker_update(&tracker, report.point, report.count, &frame);
    assert(frame.point[0].id == 15); assert(frame.point[0].x == 22); assert(frame.point[1].x == 410);
    point(data, 0, 15, 0, 22, 32); point(data, 1, 1, 6, 400, 370);
    assert(touch_report_decode(data, sizeof data, &report)); assert(report.count == 1);
    touch_tracker_update(&tracker, report.point, report.count, &frame);
    assert(!frame.point[0].down); assert(frame.point[1].down); assert(frame.point[1].id == 1);
    point(data, 0, 1, 6, 30, 40);
    assert(!touch_report_decode(data, sizeof data, &report)); assert(!report.count); // duplicate ID
    data[5] = 3;
    assert(!touch_report_decode(data, sizeof data, &report)); assert(!report.count);
    data[5] = 1; data[6] = 0;
    assert(!touch_report_decode(data, sizeof data, &report)); assert(!report.count);
    data[6] = 0xab;
    assert(!touch_report_decode(data, sizeof data - 1, &report));
    assert(!touch_report_decode(data, sizeof data + 1, &report));
    point(data, 0, 14, 1, 4095, 4095);
    assert(!touch_report_decode(data, sizeof data, &report));
    data[0] = 0xab;
    assert(touch_report_decode(data, sizeof data, &report)); assert(!report.count);
    point(data, 0, 7, 6, 40, 50); data[4] = 0x80;
    assert(touch_report_decode(data, sizeof data, &report)); assert(!report.count);
    touch_tracker_update(&tracker, report.point, report.count, &frame);
    assert(!touch_count(&frame));
    touch_tracker_update(&tracker, report.point, TOUCH_POINTS + 1, &frame);
    assert(!touch_count(&frame)); assert(!tracker.used[0]); assert(!tracker.used[1]);
}

static rub_input_t sample(input_touch_t *input, int64_t at, bool menu, bool face_touch) {
    input_touch_poll(input, at, menu, face_touch);
    rub_input_t rub;
    touch_frame_t frame;
    input_rub(&rub, &frame);
    if (valid && face_touch) assert(!memcmp(&frame, &next, sizeof frame));
    if (!face_touch) assert(!touch_count(&frame));
    return rub;
}
static void independent_fields_and_primary_gestures(void) {
    input_touch_t input = {0};
    next = (touch_frame_t){.point={{72,200,true,15}}};
    rub_input_t rub = sample(&input, 1000, true, true);
    assert(rub.down); assert(rub.x == 72); assert(drag_x == 72); assert(events[EV_DRAG] == 1);
    next.point[1] = (touch_point_t){350,350,true,1};
    rub = sample(&input, 1020, true, true);
    assert(rub.down); assert(rub.x == 72); assert(events[EV_DRAG] == 1);
    next.point[0].down = false;
    rub = sample(&input, 1040, true, true);
    assert(!rub.down); assert(!events[EV_TAP]); assert(events[EV_TOUCH_UP] == 1);
    unsigned drags = events[EV_DRAG];
    next.point[1].x = 400;
    rub = sample(&input, 1080, true, true);
    assert(!rub.down); assert(events[EV_DRAG] == drags); assert(!events[EV_TAP]);
    next = (touch_frame_t){0}; sample(&input, 1100, true, true);
    next.point[1] = (touch_point_t){300,330,true,12};
    rub = sample(&input, 1120, true, true);
    assert(rub.down); assert(rub.x == 300); assert(rub.y == 330); assert(drag_x == 300); assert(drag_y == 330);
    next = (touch_frame_t){0}; sample(&input, 1200, true, true);
    assert(events[EV_TAP] == 1); assert(events[EV_TOUCH_UP] == 2);
    next.point[0] = (touch_point_t){100,200,true,1};
    sample(&input, 1400, true, true);
    next.point[0] = (touch_point_t){400,300,true,2}; // new contact in same slot
    rub = sample(&input, 1420, true, true);
    assert(!rub.down); assert(events[EV_TAP] == 1); assert(drag_x == 100);
    next = (touch_frame_t){0}; sample(&input, 1440, true, true);
    next.point[0] = (touch_point_t){70,170,true,3};
    rub = sample(&input, 1460, true, false); // real menu never rubs or forwards game contacts
    assert(!rub.down); assert(drag_x == 70);
    next = (touch_frame_t){0}; sample(&input, 1480, true, false);
}
static void failed_samples_cancel_without_tapping(void) {
    input_touch_t input = {0};
    unsigned taps = events[EV_TAP];
    next = (touch_frame_t){.point={{100,150,true,4},{360,360,true,9}}};
    assert(sample(&input, 2000, false, true).down);
    valid = false;
    assert(sample(&input, 2020, false, true).down);
    assert(sample(&input, 2040, false, true).down);
    assert(!sample(&input, 2060, false, true).down && events[EV_TAP] == taps);
    touch_frame_t frame; rub_input_t rub; input_rub(&rub, &frame);
    assert(!touch_count(&frame));
    for (int i = 0; i < 1000; i++) sample(&input, 2080 + i * 20, false, true);
    assert(input.missed_samples == 3);
    valid = true; next = (touch_frame_t){0}; sample(&input, 23000, false, true);
    assert(!input.missed_samples);
}
static void temporary_black_holes_are_independent(void) {
    face_t face = {0}; tess_sky_t sky = {0}; sky_force_t force = {.gx=.2f, .gy=-.3f};
    sky.w.source.x = 150; sky.w.star[0].x = 350; sky.w.hole[0].x = 150;
    sky_world_t before = sky.w;
    face.touch_in = (touch_frame_t){.point={{150,250,true,15},{350,300,true,2}}};
    assert(sky_touch(&face, &sky, &force, .025f));
    assert(force.count == SKY_TOUCHES); assert(force.gx == .2f); assert(force.gy == -.3f);
    assert(sky.touch[0].shown == .25f); assert(sky.touch[1].shown == .25f);
    assert(sky.touch[0].spin == 1); assert(sky.touch[1].spin == 1);
    sky_touch(&face, &sky, &force, .075f);
    assert(sky.touch[0].shown == 1); assert(sky.touch[1].shown == 1);
    face.touch_in.point[0].down = false; face.touch_in.point[1].x = 320;
    assert(sky_touch(&face, &sky, &force, .05f));
    assert(force.count == SKY_TOUCHES); assert(!force.touch[0].down); assert(force.touch[1].down);
    assert(fabsf(force.touch[0].shown - .8f) < .0001f); assert(force.touch[1].shown == 1);
    assert(force.touch[0].x == 150); assert(force.touch[1].x == 320); assert(force.touch[1].id == 2);
    sky_touch(&face, &sky, &force, .25f);
    assert(force.touch[0].shown == 0); assert(force.touch[1].shown == 1); assert(force.count == SKY_TOUCHES);
    face.touch_in = (touch_frame_t){0};
    assert(!sky_touch(&face, &sky, &force, .25f));
    assert(!sky.finger_down); assert(force.touch[0].shown == 0); assert(force.touch[1].shown == 0);
    assert(!memcmp(&sky.w, &before, sizeof before)); // contact never moves a source, star or permanent hole
}
static void temporary_black_holes_reland_and_cancel(void) {
    face_t face = {0}; tess_sky_t sky = {0}; sky_force_t force = {0};
    face.touch_in.point[0] = (touch_point_t){150,250,true,15};
    sky_touch(&face, &sky, &force, .1f);
    face.touch_in.point[0].id = 1; // another opaque controller ID starts a fresh field in this slot
    sky_touch(&face, &sky, &force, .025f);
    assert(sky.touch[0].shown == .25f); assert(sky.touch[0].spin == 1); assert(sky.touch[0].id == 1);
    sky_touch(&face, &sky, &force, NAN);
    sky_touch(&face, &sky, &force, -1);
    assert(sky.touch[0].shown == .25f);
    face.touch_in.point[0].x = 480;
    assert(!sky_touch(&face, &sky, &force, .25f)); assert(sky.touch[0].shown == 0);
    face.touch_in.point[0].x = -1;
    assert(!sky_touch(&face, &sky, &force, .1f)); assert(sky.touch[0].shown == 0);
}
int main(void) {
    decoder_and_tracker();
    independent_fields_and_primary_gestures();
    failed_samples_cancel_without_tapping();
    temporary_black_holes_are_independent();
    temporary_black_holes_reland_and_cancel();
    puts("touch: bounded reports, arbitrary IDs, independent black holes, gesture ownership and error cancellation passed");
}
