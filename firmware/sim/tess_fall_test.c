// Offline, Tess's points fall towards the screen's own "down" (input.c hands gravity over as x right, y down, in g),
// inside the whole screen rectangle, quickly, with a small bounce and no drag like oil. cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/face.h"
#include "../main/tess_internal.h"
#include "../main/tess.h"
#include "../main/tess_fall.c"

#define DT (1.f / 30)

static face_t offline(float gx, float gy) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    for (int i = 0; i < 4 * 30; i++) face_update(&f, DT);
    f.grav_x = gx; f.grav_y = gy;
    face_set_mode(&f, MODE_OFFLINE);
    return f;
}
// The dot's place on the 480 panel, as tess_draw projects it (no view turn once fallen).
static void panel_px(const face_t *f, int i, float *x, float *y) {
    float p = 4.f / (4.7f - f->tess_position[i][2]);
    *x = 240 + 90 * f->tess_position[i][0] * p;
    *y = 255 + 90 * f->tess_position[i][1] * p;
}
static void mean_px(const face_t *f, float *mx, float *my) {
    float sx = 0, sy = 0;
    for (int i = 0; i < TESS_N; i++) { float x, y; panel_px(f, i, &x, &y); sx += x; sy += y; }
    *mx = sx / TESS_N; *my = sy / TESS_N;
}
static float worst_out;  // how far any dot has been off the panel
static void run(face_t *f, float seconds) {
    for (int i = 0; i < seconds * 30; i++) {
        face_update(f, DT);
        for (int k = 0; k < TESS_N; k++) {
            float x, y;
            panel_px(f, k, &x, &y);
            assert(isfinite(x) && isfinite(y));
            worst_out = fmaxf(worst_out, fmaxf(fmaxf(-x, x - 480), fmaxf(-y, y - 480)));
        }
    }
}

// Gravity down, right, up, left, and tilted: the heap lies against that edge (or corner) of the screen.
static void falls_where_gravity_points(void) {
    static const struct { const char *name; float gx, gy; int sx, sy; } k_case[] = {
        {"0 deg (down)", 0, 1, 0, 1}, {"90 deg (right)", 1, 0, 1, 0}, {"180 deg (up)", 0, -1, 0, -1}, {"270 deg (left)", -1, 0, -1, 0},
        {"tilt down-right", .7f, .7f, 1, 1}, {"tilt up-left", -.6f, -.8f, -1, -1}, {"tilt up-right", .5f, -.5f, 1, -1},
    };
    for (unsigned c = 0; c < sizeof k_case / sizeof k_case[0]; c++) {
        face_t f = offline(k_case[c].gx, k_case[c].gy);
        run(&f, 3);
        float mx, my;
        mean_px(&f, &mx, &my);
        printf("gravity %-16s: heap at %.0f, %.0f px\n", k_case[c].name, mx, my);
        assert(f.tess_fallen);
        if (k_case[c].sx) assert((mx - 240) * k_case[c].sx > 120); else assert(fabsf(mx - 240) < 90);
        if (k_case[c].sy) assert((my - 240) * k_case[c].sy > 120); else assert(fabsf(my - 240) < 90);
    }
}

// Points at the floor (y > 455: the pile's bottom row) that later come back up: bounce samples; also when 80% are below y 400.
static void watch_fall(face_t *f, float *bottom_at, int *bounced) {
    static bool hit[TESS_N];
    *bottom_at = -1;
    for (int i = 0; i < 3 * 30; i++) {
        face_update(f, DT);
        int low = 0;
        for (int k = 0; k < TESS_N; k++) {
            float x, y;
            panel_px(f, k, &x, &y);
            if (y > 455) hit[k] = true;
            if (hit[k] && y < 440 && f->tess_velocity[k][1] < -1.f) (*bounced)++;
            low += y > 400;
        }
        if (*bottom_at < 0 && low >= TESS_N * 8 / 10) *bottom_at = (i + 1) * DT;
    }
}

// From the middle of the cube to the floor in a third of a second; a bounce off it; rolling to the other side quickly.
static void moves_like_things_do(void) {
    face_t f = offline(0, 1);
    float bottom_at, my, mx;
    int bounced = 0;
    watch_fall(&f, &bottom_at, &bounced);
    printf("fall: 80%% of the points below y 400 after %.2f s, %d bounce samples\n", bottom_at, bounced);
    assert(bottom_at > .25f && bottom_at < .6f);  // about a third of a second
    assert(bounced > 20);                           // and they do bounce
    run(&f, 1);
    f.grav_x = 1; f.grav_y = 0;
    float to_wall = -1;
    for (int i = 0; i < 4 * 30; i++) {
        face_update(&f, DT);
        mean_px(&f, &mx, &my);
        if (to_wall < 0 && mx > 390) to_wall = (i + 1) * DT;
    }
    printf("roll: mean x reaches 390 px %.2f s after gravity turns to the right\n", to_wall);
    assert(to_wall > 0 && to_wall < 1.4f);  // not through oil
}

// Same inputs, same fall; and the fall never leaves the panel, corners included.
static void deterministic_and_inside(void) {
    face_t a = offline(.3f, .95f), b = offline(.3f, .95f);
    for (int i = 0; i < 6 * 30; i++) {
        float t = i * DT;
        a.grav_x = b.grav_x = sinf(t * 1.7f); a.grav_y = b.grav_y = cosf(t * 1.7f);
        face_update(&a, DT); face_update(&b, DT);
    }
    assert(!memcmp(a.tess_position, b.tess_position, sizeof a.tess_position));
    face_t f = offline(1, 1);
    run(&f, 3);
    face_t g = offline(-1, -1);
    run(&g, 3);
    printf("bounds: no dot ever %.1f px outside the panel\n", worst_out);
    assert(worst_out < 1.f);
}

static int impact_cues(face_t *f, int frames) {
    int count = 0;
    tess_cue_t cue;
    float strength, identity;
    for (int i = 0; i < frames; i++) {
        face_update(f, DT);
        while (face_take_cue(f, &cue, &strength, &identity)) {
            assert(cue == TC_IMPACT && strength >= 0 && strength <= 1 && fabsf(identity) <= 1);
            count++;
        }
    }
    return count;
}

static void motion_sound_and_hidden_character(void) {
    face_t f = offline(0, 1);
    assert(impact_cues(&f, 90) > 0);
    impact_cues(&f, 300);
    assert(impact_cues(&f, 90) == 0);
    float turn = f.tess_xw;
    tess_play_t play = f.tess_play;
    tess_event(&f, FEV_TAP, .5f, .5f);
    tess_event(&f, FEV_SHAKE, .5f, .5f);
    tess_emotion(&f, EMO_JOY, 2);
    assert(!memcmp(&play, &f.tess_play, sizeof play));
    assert(impact_cues(&f, 30) == 0 && f.tess_xw == turn);
    f.grav_x = .02f;
    assert(impact_cues(&f, 90) == 0);
    f.grav_x = 1; f.grav_y = 0;
    int notes = impact_cues(&f, 60);
    assert(notes > 9 && notes <= 60 * TESS_CUES);
    face_set_mode(&f, MODE_IDLE);
    face_update(&f, DT);
    assert(!f.tess_fallen && f.tess_xw == turn);
}

static void only_closing_contacts_have_impact(void) {
    fall_points_t s = {0};
    s.q[1][0] = FALL_DIAMETER / 2;
    s.v[0][0] = s.v[1][0] = 10 * FALL_SCALE;
    separate_pair(&s, 0, 1);
    assert(s.touch[0] && s.impact[0] == 0);  // fast parallel motion is silent
    s.q[0][0] = 0; s.q[1][0] = FALL_DIAMETER / 2;
    s.v[0][0] = -10 * FALL_SCALE;
    separate_pair(&s, 0, 1);
    assert(s.impact[0] == 0);  // separating neighbours are silent too
    s.q[0][0] = 0; s.q[1][0] = FALL_DIAMETER / 2;
    s.v[0][0] = 20 * FALL_SCALE;
    separate_pair(&s, 0, 1);
    assert(s.impact[0] == 10 * FALL_SCALE && s.impact[1] == 10 * FALL_SCALE);
}

static void only_incoming_wall_contacts_have_impact(void) {
    for (int axis = 0; axis < 2; axis++) {
        for (int edge = -1; edge <= 1; edge += 2) {
            fall_points_t s = {0};
            s.edge[0][axis] = edge;
            s.v[0][axis] = edge * 10 * FALL_SCALE;
            update_fall_velocity(&s, 60);
            assert(s.impact[0] == 0);  // moving away from this wall is silent
            s.v[0][axis] = -edge * 10 * FALL_SCALE;
            update_fall_velocity(&s, 60);
            assert(s.impact[0] == 10 * FALL_SCALE);
        }
    }
}

static void continuous_transitions(void) {
    for (int pose = 0; pose < 5; pose++) {
        face_t f; face_init(&f); face_set_character(&f, CHARACTER_TESS);
        face_set_mode(&f, pose & 1 ? MODE_THINKING : MODE_IDLE);
        for (int n = 0; n < 120; n++) face_update(&f, DT);
        f.style.cam = pose & 1 ? 3.9f : 5.5f;
        f.view_q[0] = .8f; f.view_q[1] = .3f; f.view_q[2] = .4f;
        f.tess_mood.drag_angle[0] = pose * .7f;
        f.tess_mood.drag_angle[1] = pose * -.4f;
        tess_projected_t before[TESS_N], after[TESS_N];
        tess_project_dots(&f, before);
        tess_fall_turn(&f, true); tess_project_dots(&f, after);
        for (int i = 0; i < TESS_N; i++) {
            assert(abs(before[i].x - after[i].x) <= 2);
            assert(abs(before[i].y - after[i].y) <= 2);
        }
        assert(f.tess_rigid == 0);
        for (int n = 0; n < 90; n++) tess_fall_step(&f, DT);
        tess_project_dots(&f, before);
        tess_fall_turn(&f, false); tess_project_dots(&f, after);
        for (int i = 0; i < TESS_N; i++) {
            assert(abs(before[i].x - after[i].x) <= 2);
            assert(abs(before[i].y - after[i].y) <= 2);
        }
        tess_fall_turn(&f, true); f.tess_rigid = 1; f.mode = MODE_IDLE;
        tess_update(&f, .001f); assert(!f.tess_fallen && f.tess_rigid < .1f);
    }
    puts("fall transitions: every point's projected centre continuous in both directions; no rigid snap");
}

int main(void) {
    continuous_transitions();
    falls_where_gravity_points();
    moves_like_things_do();
    deterministic_and_inside();
    motion_sound_and_hidden_character();
    only_closing_contacts_have_impact();
    only_incoming_wall_contacts_have_impact();
    puts("tess fall ok");
    return 0;
}
