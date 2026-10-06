// A press sends a wave through the dots: a front runs out from the finger at a set speed (across the cube in about a
// third of a second), pushes each dot away from the finger as it passes, weaker with distance and time, the springs
// let the dots overshoot a little, and afterwards every dot is exactly on its place again. A few waves at once add up,
// but never more than three. cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "play_script.h"

#define DT (1.f / 30)

static face_t rested(void) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    f.tess_next_fidget = 1e9f;
    for (int i = 0; i < 8 * 30; i++) face_update(&f, DT);
    return f;
}

// Where the dots are asked to be without the waves, and with them.
static void targets(face_t *f, float plain[TESS_N][3], float waved[TESS_N][3]) {
    tess_ripple_t keep[TESS_RIPPLES];
    memcpy(keep, f->tess_ripple, sizeof keep);
    for (int k = 0; k < TESS_RIPPLES; k++) f->tess_ripple[k].age = 99;
    tess_point_targets(f, plain, -1);
    memcpy(f->tess_ripple, keep, sizeof keep);
    tess_point_targets(f, waved, -1);
}

static float push_of(const float plain[3], const float waved[3]) {
    return sqrtf((waved[0] - plain[0]) * (waved[0] - plain[0]) + (waved[1] - plain[1]) * (waved[1] - plain[1]));
}

static float distance_from(const float dot[3], float x, float y) { return hypotf(dot[0] - x, dot[1] - y); }

static int farthest_dot(const face_t *f, float x, float y) {
    int last = 0;
    for (int i = 1; i < TESS_N; i++)
        if (distance_from(f->tess_target[i], x, y) > distance_from(f->tess_target[last], x, y)) last = i;
    return last;
}

// What a wave of this age does to the dots: how far out it reaches, its strongest push, and the push on one dot.
typedef struct { float reach, peak, on_dot; } wave_t;

static wave_t wave_at(face_t *f, float x, float y, float age, int dot) {
    static float plain[TESS_N][3], waved[TESS_N][3];
    wave_t w = {0};
    f->tess_ripple[0] = (tess_ripple_t){x, y, age};
    targets(f, plain, waved);
    for (int i = 0; i < TESS_N; i++) {
        float push = push_of(plain[i], waved[i]);
        if (i == dot) w.on_dot = push;
        if (push < 1e-4f) continue;
        w.reach = fmaxf(w.reach, distance_from(plain[i], x, y));
        w.peak = fmaxf(w.peak, push);
        assert(waved[i][2] > plain[i][2]);  // towards the viewer
        assert((waved[i][0] - plain[i][0]) * (plain[i][0] - x) + (waved[i][1] - plain[i][1]) * (plain[i][1] - y) > 0);  // away from the finger
    }
    return w;
}

// The front: how far out the pushed dots reach as it ages (the speed), when it passes the dot farthest from the finger
// (it has crossed the whole cube), and that it weakens and ends.
static void the_front_runs_at_a_set_speed(void) {
    face_t f = rested();
    float x = -.5f, y = .3f;
    int last = farthest_dot(&f, x, y);
    float far_dot = distance_from(f.tess_target[last], x, y);
    float reach_at[2] = {0}, last_peak = 0, peak_at = 0, peak_early = 0, peak_late = 0, over = 0;
    for (int n = 1; n <= 40; n++) {
        float age = n * DT / 2;
        wave_t w = wave_at(&f, x, y, age, last);
        over = w.reach > 0 ? age : over;
        peak_at = w.on_dot > last_peak ? age : peak_at;
        last_peak = fmaxf(last_peak, w.on_dot);
        if (n == 9) reach_at[0] = w.reach;   // .15 s
        if (n == 18) reach_at[1] = w.reach;  // .3 s
        peak_early = fmaxf(peak_early, age < .3f ? w.peak : 0);
        peak_late = fmaxf(peak_late, age > .6f ? w.peak : 0);
    }
    float speed = (reach_at[1] - reach_at[0]) / .15f;
    printf("ripple front: %.1f units/s; it passes the dot farthest from the finger (%.2f units) at %.2f s; peak push %.3f early, %.3f late, over after %.2f s\n",
           speed, far_dot, peak_at, peak_early, peak_late, over);
    assert(speed > 4.5f && speed < 8.5f);
    assert(peak_at > .25f && peak_at < .4f);            // across the cube in a third of a second
    assert(peak_late < .6f * peak_early && over < 1.f);  // it weakens and ends
    assert(peak_early > .02f && peak_early < .4f);     // felt, but the cube stays a cube
}

// What each dot did in the wave of a tap, against the same face without the wave (all else is the same): when it was
// first moved and how far from the finger it then was, how far out the springs took it, and how far back beyond its place.
typedef struct { float first_push, at_distance, peak_out, dip_back; } dot_t;

static void note_dots(dot_t dot[TESS_N], const face_t *f, const face_t *twin, float t, float px, float py) {
    for (int i = 0; i < TESS_N; i++) {
        const float *rest = twin->tess_position[i], *now = f->tess_position[i];
        float dx = rest[0] - px, dy = rest[1] - py, d = hypotf(dx, dy) + 1e-4f;
        float out = ((now[0] - rest[0]) * dx + (now[1] - rest[1]) * dy) / d;
        if (dot[i].first_push < 0 && fabsf(out) > 1e-3f) { dot[i].first_push = t; dot[i].at_distance = d; }
        dot[i].peak_out = fmaxf(dot[i].peak_out, out);
        if (dot[i].peak_out > .01f) dot[i].dip_back = fminf(dot[i].dip_back, out);
    }
}

// How well the time of the first push follows the distance from the finger (1: the farther, the later).
static float order_of_arrival(const dot_t dot[TESS_N]) {
    float mt = 0, md = 0, n = 0, ct = 0, cd = 0, cross = 0;
    for (int i = 0; i < TESS_N; i++)
        if (dot[i].first_push >= 0) { mt += dot[i].first_push; md += dot[i].at_distance; n++; }
    mt /= n; md /= n;
    for (int i = 0; i < TESS_N; i++) {
        if (dot[i].first_push < 0) continue;
        ct += (dot[i].first_push - mt) * (dot[i].first_push - mt);
        cd += (dot[i].at_distance - md) * (dot[i].at_distance - md);
        cross += (dot[i].first_push - mt) * (dot[i].at_distance - md);
    }
    return cross / sqrtf(ct * cd);
}

static void count_dots(const dot_t dot[TESS_N], int *reached, int *pushed, int *overshoot, float *biggest) {
    for (int i = 0; i < TESS_N; i++) {
        *reached += dot[i].first_push >= 0;
        *biggest = fmaxf(*biggest, dot[i].peak_out);
        *pushed += dot[i].peak_out > .01f;
        *overshoot += dot[i].peak_out > .01f && dot[i].dip_back < -.002f;
    }
}

// The tap of the test: four seconds with its wave, beside a twin whose wave is put out of reach at once (the same springs
// and the same everything else, but no push): the dots' difference is the wave's doing alone.
static void follow_tap(face_t *f, dot_t dot[TESS_N], float *started, float *ended) {
    face_t twin = *f;
    finger_t finger = {0}, twin_finger = {0};
    static const stroke_t tap[] = {{.2f, .3f, 170, 200, 170, 200}};
    float px = (170 - 240) / TESS_PX_PER_UNIT, py = (200 - 255) / TESS_PX_PER_UNIT;
    *started = *ended = -1;
    for (int i = 0; i < TESS_N; i++) dot[i] = (dot_t){-1, 0, 0, 0};
    for (int n = 0; n < 4 * 30; n++) {
        float t = n * DT;
        play_feed(f, &finger, tap, 1, t);
        play_feed(&twin, &twin_finger, tap, 1, t);
        face_update(f, DT);
        face_update(&twin, DT);
        for (int k = 0; k < TESS_RIPPLES; k++)
            if (twin.tess_ripple[k].age < 1.f) twin.tess_ripple[k].age = .85f;
        bool active = tess_ripple_active(f);
        *started = active && *started < 0 ? t : *started;
        *ended = !active && *started >= 0 && *ended < 0 ? t : *ended;
        if (active) note_dots(dot, f, &twin, t - *started, px, py);
    }
}

// A tap on the front of the cube: nearer dots are moved first, the springs carry the dots out and beyond their places, and
// at the end every dot sits exactly on its place.
static void a_tap_sends_a_wave_and_it_settles(void) {
    face_t f = rested();
    dot_t dot[TESS_N];
    float started, ended, biggest = 0, order;
    int overshoot = 0, pushed = 0, reached = 0;
    follow_tap(&f, dot, &started, &ended);
    order = order_of_arrival(dot);
    count_dots(dot, &reached, &pushed, &overshoot, &biggest);
    printf("ripple tap: wave from %.2f s to %.2f s, %d dots reached, time against distance correlates %.2f, %d dots pushed out, %d of them overshoot; "
           "at most %.3f units (%.0f px) from its place; at rest again: rigid %.2f\n",
           started, ended, reached, order, pushed, overshoot, biggest, biggest * TESS_PX_PER_UNIT, f.tess_rigid);
    assert(started >= .2f && started < .2f + 2 * DT);  // (in the frame the finger lands)
    assert(reached > TESS_N * 2 / 3 && order > .9f);
    assert(pushed > 20 && overshoot > 5);
    assert(f.tess_rigid >= 1 && !tess_ripple_active(&f) && !memcmp(f.tess_position, f.tess_target, sizeof f.tess_target));
}

// Taps in quick succession: the waves add up, three at most, and the oldest goes when a fourth comes.
static void waves_add_up_to_three(void) {
    face_t f = rested();
    finger_t finger = {0};
    static const stroke_t taps[] = {{.1f, .15f, 200, 250, 200, 250}, {.3f, .35f, 280, 260, 280, 260}, {.5f, .55f, 240, 200, 240, 200},
                                    {.7f, .75f, 210, 300, 210, 300}, {.9f, .95f, 260, 240, 260, 240}};
    int most = 0;
    float peak_one = 0, peak_all = 0, plain[TESS_N][3], waved[TESS_N][3];
    for (int n = 0; n < 60; n++) {
        play_feed(&f, &finger, taps, 5, n * DT);
        face_update(&f, DT);
        int active = 0;
        for (int k = 0; k < TESS_RIPPLES; k++) active += f.tess_ripple[k].age < 99;
        most = active > most ? active : most;
        if (n == 5) {  // one wave: its push
            targets(&f, plain, waved);
            for (int i = 0; i < TESS_N; i++) peak_one = fmaxf(peak_one, push_of(plain[i], waved[i]));
        }
        if (n >= 20 && n < 30) {
            targets(&f, plain, waved);
            for (int i = 0; i < TESS_N; i++) peak_all = fmaxf(peak_all, push_of(plain[i], waved[i]));
        }
    }
    printf("ripples: up to %d waves at once, peak push %.3f with one, %.3f with several\n", most, peak_one, peak_all);
    assert(most == TESS_RIPPLES && TESS_RIPPLES == 3);
    assert(peak_all > peak_one * .9f && peak_all < .5f);
}

int main(void) {
    the_front_runs_at_a_set_speed();
    a_tap_sends_a_wave_and_it_settles();
    waves_add_up_to_three();
    puts("tess ripple ok");
    return 0;
}
