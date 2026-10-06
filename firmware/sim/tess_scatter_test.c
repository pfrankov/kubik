// The burst of a hard shake (tess_scatter.c): the dots fly out over the whole panel and ease to a stop, shiver in place, each
// its own way, and are pulled home like by a magnet (faster and faster, a small overshoot) until they sit exactly on the
// tesseract again. A small shake only trembles the dots. cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/tess_internal.h"

#define DT (1.f / 30)
#define FRAMES 120  // the burst is 2.7 s; four seconds watched

typedef struct { float px[FRAMES][TESS_N][3], home[FRAMES][TESS_N][3]; float sx[FRAMES][TESS_N], sy[FRAMES][TESS_N]; bool bursting[FRAMES]; bool exact[FRAMES]; float startle_at, snap_at; int snaps; } run_t;

// Where the dot is on the panel (px), the way tess_draw projects it.
static void on_panel(const face_t *f, const float p[3], float *x, float *y) {
    float m[3][3], v[3];
    tess_view_matrix(f, m);
    tess_transform_point(m, p, v, false);
    float scale = 4.f / (4.7f - v[2]);
    *x = 240 + 90 * scale * v[0];
    *y = 255 + 90 * scale * v[1];
}

static face_t rested(void) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    f.tess_next_fidget = 1e9f;
    for (int i = 0; i < 8 * 30; i++) face_update(&f, DT);
    assert(f.tess_rigid >= 1);
    return f;
}

// Frame k is k + 1 frames after the event: seconds into the burst (k + 1) / 30.
static void record(face_t *f, run_t *r, bool hard, float jolt) {
    if (hard) face_event(f, FEV_SHAKE, 0, 0);
    for (int k = 0; k < FRAMES; k++) {
        if (jolt != 0 && k < 90 && k % 5 == 0) f->jolt_dvx = k / 5 % 2 ? jolt : -jolt;
        face_update(f, DT);
        r->bursting[k] = f->tess_hold[TR_SCATTER] > 0;
        memcpy(r->px[k], f->tess_position, sizeof f->tess_position);
        face_t rest = *f;  // where the dots would be without the burst
        rest.tess_hold[TR_SCATTER] = 0;
        tess_point_targets(&rest, r->home[k], -1);
        r->exact[k] = !memcmp(r->px[k], r->home[k], sizeof r->px[k]);
        tess_cue_t cue;
        float strength, position;
        while (face_take_cue(f, &cue, &strength, &position)) {
            if (cue == TC_STARTLE) r->startle_at = (k + 1) / 30.f;
            if (cue == TC_SNAP) { r->snap_at = (k + 1) / 30.f; r->snaps++; }
        }
        for (int i = 0; i < TESS_N; i++) on_panel(f, f->tess_position[i], &r->sx[k][i], &r->sy[k][i]);
    }
}

static void extent(const run_t *r, int k, float *width, float *height) {
    float lo_x = 1e9f, hi_x = -1e9f, lo_y = 1e9f, hi_y = -1e9f;
    for (int i = 0; i < TESS_N; i++) {
        lo_x = fminf(lo_x, r->sx[k][i]); hi_x = fmaxf(hi_x, r->sx[k][i]);
        lo_y = fminf(lo_y, r->sy[k][i]); hi_y = fmaxf(hi_y, r->sy[k][i]);
    }
    *width = hi_x - lo_x; *height = hi_y - lo_y;
}

static float step_units(const run_t *r, int k, int i) {  // how far the dot moved in frame k
    float d = 0;
    for (int a = 0; a < 3; a++) d += (r->px[k][i][a] - r->px[k - 1][i][a]) * (r->px[k][i][a] - r->px[k - 1][i][a]);
    return sqrtf(d);
}

static void it_fills_the_whole_screen(const run_t *r) {
    float best_w = 0, best_h = 0, margin = 1e9f;
    for (int k = 0; k < FRAMES; k++) {
        float w, h;
        extent(r, k, &w, &h);
        if (w > best_w) best_w = w;
        if (h > best_h) best_h = h;
        if (!r->bursting[k]) continue;
        for (int i = 0; i < TESS_N; i++) margin = fminf(margin, fminf(fminf(r->sx[k][i], 480 - r->sx[k][i]), fminf(r->sy[k][i], 480 - r->sy[k][i])));
    }
    printf("burst: the dots span %.0f x %.0f px of the 480 x 480 panel, nearest to an edge %.1f px\n", best_w, best_h, margin);
    assert(best_w >= .9f * 480 && best_h >= .9f * 480);
    assert(margin > 0);  // all of them stay on the panel
}

// From 0.5 s to 0.8 s (the last dot's flight ends, and the shiver starts) every dot only ever slows down, but for gravity's
// slow bend of its way (a fraction of a pixel a frame): no abrupt stop.
static void the_flight_eases_out(const run_t *r) {
    float fastest_end = 0, fastest_start = 0;
    for (int i = 0; i < TESS_N; i++) {
        int first = 14, last = 23;  // frames k: seconds (k + 1) / 30
        fastest_start = fmaxf(fastest_start, step_units(r, 4, i));
        for (int k = first + 1; k <= last; k++) assert(step_units(r, k, i) <= step_units(r, k - 1, i) + .003f);
        fastest_end = fmaxf(fastest_end, step_units(r, last, i));
    }
    printf("flight: the fastest dot moves %.3f units a frame at the start; none moves more than %.4f at the end\n", fastest_start, fastest_end);
    assert(fastest_start > .25f && fastest_end < .01f);
}

#define APART 5

// Between 1.0 s and 1.5 s the dots shiver: a jitter of every dot, and not the same jitter.
static void the_dots_struggle_each_on_its_own(const run_t *r) {
    static float kick[TESS_N][3][14];  // the change of the step from frame to frame (the shiver; the slow pull is left out)
    int first = 30, n = 14;
    float weakest = 1e9f;
    for (int i = 0; i < TESS_N; i++) {
        float sum = 0;
        for (int c = 0; c < n; c++)
            for (int a = 0; a < 3; a++) {
                kick[i][a][c] = r->px[first + c + 1][i][a] - 2 * r->px[first + c][i][a] + r->px[first + c - 1][i][a];
                sum += kick[i][a][c] * kick[i][a][c];
            }
        weakest = fminf(weakest, sqrtf(sum / (3 * n)));
    }
    float agree = 0, size = 0;
    for (int i = 0; i + APART < TESS_N; i++)  // (dots next to each other have spots next to each other, and are pulled alike)
        for (int c = 0; c < n; c++) {
            agree += kick[i][0][c] * kick[i + APART][0][c] + kick[i][1][c] * kick[i + APART][1][c];
            size += kick[i][0][c] * kick[i][0][c] + kick[i][1][c] * kick[i][1][c];
        }
    printf("struggle: the weakest dot shivers by %.3f units a frame; the shivers of dots %d apart agree %.0f%%\n", weakest, APART, 100 * fabsf(agree / size));
    assert(weakest > .01f);
    assert(fabsf(agree / size) < .15f);
}

static float mean_speed(const run_t *r, int from, int to) {
    float sum = 0;
    for (int k = from; k <= to; k++)
        for (int i = 0; i < TESS_N; i++) sum += step_units(r, k, i);
    return sum / ((to - from + 1) * TESS_N);
}

static void the_return_pulls_harder_as_it_closes_in(const run_t *r) {
    // frames k: seconds (k + 1) / 30. The dots are let go from 1.4 s to 1.6 s, and are home 0.5 s later.
    float early = mean_speed(r, 44, 49), middle = mean_speed(r, 51, 54), late = mean_speed(r, 56, 58);
    printf("return: mean speed %.3f, then %.3f, then %.3f units a frame\n", early, middle, late);
    assert(early < middle && middle < late && late > 5 * early);
    int overshooting = 0, ringing = 0;
    for (int i = 0; i < TESS_N; i++) {  // it goes past the place, on the far side of where it came from, and back again
        float came[3], side = 1;
        int swings = 0;
        for (int a = 0; a < 3; a++) came[a] = r->px[46][i][a] - r->home[46][i][a];
        for (int k = 50; k < 90; k++) {
            float along = 0;
            for (int a = 0; a < 3; a++) along += (r->px[k][i][a] - r->home[k][i][a]) * came[a];
            if (along * side < -1e-4f) { side = -side; swings++; }
        }
        overshooting += swings >= 1;
        ringing += swings >= 2;
    }
    printf("return: %d of %d dots overshoot their place, %d swing back through it\n", overshooting, TESS_N, ringing);
    assert(overshooting > TESS_N * 9 / 10 && ringing > TESS_N / 2);
}

// The screen's gravity bends the flight and the dots hanging there, a little: turn the device over and they lean the other way.
static void gravity_bends_the_flight(const run_t *up, const run_t *down) {
    int k = 35;  // 1.2 s: hanging in the strain
    float lean = 0, sideways = 0;
    for (int i = 0; i < TESS_N; i++) {
        lean += up->sy[k][i] - down->sy[k][i];
        sideways += up->sx[k][i] - down->sx[k][i];
    }
    lean /= TESS_N; sideways /= TESS_N;
    printf("gravity: turned over, the dots hang %.1f px higher and %.1f px to the side\n", lean, sideways);
    assert(lean > 10 && fabsf(sideways) < 1);
}

static void it_ends_exactly_on_the_tesseract(const run_t *r, const face_t *f) {
    int done = 0;
    for (int k = 0; k < FRAMES; k++)
        if (r->bursting[k] && !r->exact[k]) done = k + 1;
    float seconds = (done + 1) / 30.f;
    printf("landing: exactly home from %.2f s on, the burst is over at %.2f s\n", seconds, TESS_BURST_S);
    assert(done < FRAMES && r->bursting[done] && seconds > 2.f && seconds < 3.f);
    assert(f->tess_rigid >= 1 && !f->tess_hold[TR_SCATTER] && f->tess_reaction[TR_SCATTER] == 0);
    assert(!memcmp(f->tess_position, f->tess_target, sizeof f->tess_target));
}

// The sound of the shake, then the magnet's snap as the dots close on their places.
static void the_magnet_snaps(const run_t *r) {
    printf("sound: startle at %.2f s, snap at %.2f s\n", r->startle_at, r->snap_at);
    assert(r->snaps == 1 && r->startle_at < .2f && r->snap_at > 1.8f && r->snap_at < 2.2f);
}

static void a_small_shake_does_not_scatter(void) {
    static run_t r;
    face_t f = rested();
    record(&f, &r, false, .035f);
    float width, height, widest = 0;
    for (int k = 0; k < FRAMES; k++) {
        assert(!r.bursting[k] && f.tess_reaction[TR_SCATTER] == 0);
        extent(&r, k, &width, &height);
        widest = fmaxf(widest, fmaxf(width, height));
    }
    printf("small shake: no burst, the cloud never wider than %.0f px\n", widest);
    assert(widest < .7f * 480);  // (a startled Tess comes closer: curious, it is a little larger; a burst is 444)
}

int main(void) {
    static run_t r, upside_down;
    face_t f = rested();
    record(&f, &r, true, 0);
    face_t g = rested();
    g.grav_y = -1;
    record(&g, &upside_down, true, 0);
    static run_t neutral;
    face_t n = rested();
    face_mood_off(&n, true);
    record(&n, &neutral, true, 0);
    float worst = 0;
    for (int k = 0; k < FRAMES; k++)
        for (int i = 0; i < TESS_N; i++)
            for (int a = 0; a < 3; a++) { float d = fabsf(r.px[k][i][a] - neutral.px[k][i][a]); if (r.bursting[k]) worst = fmaxf(worst, d); }
    printf("moods: in the burst the dots are off the neutral Tess's by %.3f units (%.1f px) at most\n", worst, worst * 77);
    assert(worst < .05f);
    it_fills_the_whole_screen(&r);
    the_flight_eases_out(&r);
    the_dots_struggle_each_on_its_own(&r);
    the_return_pulls_harder_as_it_closes_in(&r);
    it_ends_exactly_on_the_tesseract(&r, &n);  // (a frightened Tess trembles once it is whole: the neutral one is still)
    gravity_bends_the_flight(&r, &upside_down);
    the_magnet_snaps(&r);
    a_small_shake_does_not_scatter();
    puts("tess scatter ok");
    return 0;
}
