// What Tess does when touched and when left alone: the touch is answered in the very frame it lands, a swipe is carried and
// swung back, repeated taps build up, a neglected cloud dodges and is glad to be caught, and it asks to be noticed and gets up
// to mischief when ignored, all as turns and shifts of a rigid body. The cues (what the speaker plays) are read as
// the app does. cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "play_script.h"

#define DT (1.f / 30)
#define MAX_CUES 64

typedef struct { float t; tess_cue_t cue; float strength, position; } heard_t;
typedef struct { face_t face; finger_t finger; heard_t heard[MAX_CUES]; int heard_n; float t; int n; } run_t;

static const stroke_t k_tap[] = {{.5f, .6f, 340, 255, 340, 255}};
static const stroke_t k_swipe[] = {{.5f, .75f, 130, 255, 350, 255}};
static const stroke_t k_taps[] = {{.4f, .5f, 250, 250, 250, 250}, {.9f, 1.f, 270, 260, 270, 260}, {1.4f, 1.5f, 240, 250, 240, 250},
                                  {1.9f, 2.f, 260, 260, 260, 260}, {2.4f, 2.5f, 250, 250, 250, 250}, {2.9f, 3.f, 240, 260, 240, 260}};
static const stroke_t k_catch[] = {{.5f, .6f, 330, 255, 330, 255}, {1.6f, 1.7f, 300, 255, 300, 255}};
static const stroke_t k_answer[] = {{4.f, 4.1f, 260, 255, 260, 255}};

static run_t *started(void) {
    static run_t r;
    memset(&r, 0, sizeof r);
    face_init(&r.face);
    face_set_character(&r.face, CHARACTER_TESS);
    face_set_mode(&r.face, MODE_IDLE);
    for (int i = 0; i < 8 * 30; i++) face_update(&r.face, DT);
    return &r;
}

// Rigid: every pairwise 4D distance of the 16 vertices stays that of the unit tesseract.
static float worst_distance;
static void check_rigid(const face_t *f) {
    float q[16][4];
    tess_vertices4d(f, q);
    for (int a = 0; a < 16; a++)
        for (int b = a + 1; b < 16; b++) {
            float d = 0, want = 4.f * __builtin_popcount(a ^ b);
            for (int k = 0; k < 4; k++) d += (q[a][k] - q[b][k]) * (q[a][k] - q[b][k]);
            worst_distance = fmaxf(worst_distance, fabsf(d - want));
        }
}

static void cloud_px(const face_t *f, float *x, float *y) {
    float sx = 0, sy = 0;
    for (int i = 0; i < TESS_N; i++) {
        float p = 4.f / (4.7f - f->tess_position[i][2]);
        sx += 240 + 90 * f->tess_position[i][0] * p; sy += 255 + 90 * f->tess_position[i][1] * p;
    }
    *x = sx / TESS_N; *y = sy / TESS_N;
}

// One frame, with the finger of these strokes (or none), and the cues heard after it.
static void frame(run_t *r, const stroke_t *strokes, int count) {
    if (strokes) play_feed(&r->face, &r->finger, strokes, count, r->t);
    face_update(&r->face, DT);
    check_rigid(&r->face);
    heard_t h = {.t = r->t};
    while (face_take_cue(&r->face, &h.cue, &h.strength, &h.position))
        if (r->heard_n < MAX_CUES) r->heard[r->heard_n++] = h;
    assert(r->face.tess_play.cue_n == 0);
    r->t = ++r->n * DT;
}

static void run_for(run_t *r, float seconds, const stroke_t *strokes, int count) {
    float until = r->t + seconds;
    while (r->t < until - 1e-4f) frame(r, strokes, count);
}
#define RUN(r, s, a) run_for(r, s, a, (int)(sizeof a / sizeof a[0]))

static int nth_cue(const run_t *r, tess_cue_t cue, int n) {
    for (int i = 0; i < r->heard_n; i++)
        if (r->heard[i].cue == cue && !n--) return i;
    return -1;
}
static int cues_of(const run_t *r, tess_cue_t cue) {
    int n = 0;
    for (int i = 0; i < r->heard_n; i++) n += r->heard[i].cue == cue;
    return n;
}

// Touch it and the answer is there in the frame the finger lands: a sound, and a cloud that has already moved.
static void answers_within_a_frame(void) {
    static const stroke_t k_rim[] = {{.5f, .6f, 410, 330, 410, 330}}, k_top[] = {{.5f, .6f, 150, 120, 150, 120}};
    const struct { const stroke_t *s; int n; } cases[] = {{k_tap, 1}, {k_rim, 1}, {k_top, 1}};
    for (int c = 0; c < 3; c++) {
        run_t *r = started();
        float x0, y0, x1, y1, worst = 0;
        cloud_px(&r->face, &x0, &y0);
        while (!cues_of(r, TC_TOUCH)) frame(r, cases[c].s, cases[c].n);
        assert(r->finger.down && r->finger.down_s < .05f);  // the very frame in which the finger landed
        for (int i = 0; i < 12; i++) {
            cloud_px(&r->face, &x1, &y1);
            worst = fmaxf(worst, hypotf(x1 - x0, y1 - y0));
            frame(r, cases[c].s, cases[c].n);
        }
        cloud_px(&r->face, &x1, &y1);
        printf("touch %d: sound and first movement in the landing frame; the cloud moved %.0f px within .4 s\n", c, worst);
        assert(worst > 6.f);
    }
}

// A quick swipe carries the cloud round (fling), and after a moment it swings back the way it came.
static void swipe_is_carried_and_swings_back(void) {
    run_t *r = started();
    float phase0 = r->face.tess_phase, peak = 0, end = 0;
    for (int i = 0; i < 5 * 30; i++) {
        frame(r, k_swipe, 1);
        peak = fmaxf(peak, r->face.tess_phase + r->face.tess_mood.drag_angle[0] - phase0);
    }
    end = r->face.tess_phase + r->face.tess_mood.drag_angle[0] - phase0;
    int fling = nth_cue(r, TC_FLING, 0), swing = nth_cue(r, TC_SWING, 0);
    printf("swipe: carried %+.2f rad, then swung back to %+.2f; fling at %.2f s, swing-back at %.2f s\n", peak, end, r->heard[fling].t, r->heard[swing].t);
    assert(fling >= 0 && swing > fling);
    assert(r->heard[fling].position * r->heard[swing].position < 0);  // the other way
    assert(peak > 1.5f && peak - end > 1.f);
    assert(r->heard[swing].t - r->heard[fling].t > .7f && r->heard[swing].t - r->heard[fling].t < 1.6f);
}

// Every direction follows the finger, then carries on and slows down.
static void swipes_in_all_directions(void) {
    const int directions[][2] = {{0,1},{0,-1},{1,0},{-1,0},{1,1},{-1,-1},{1,-1},{-1,1}};
    for (unsigned i = 0; i < sizeof directions / sizeof directions[0]; i++) {
        int dx = directions[i][0], dy = directions[i][1];
        stroke_t stroke = {.5f,.75f,240-100*dx,240-100*dy,240+100*dx,240+100*dy};
        run_t *r = started();
        run_for(r, .8f, &stroke, 1);
        float before[2] = {r->face.tess_mood.drag_angle[0], r->face.tess_mood.drag_angle[1]};
        assert(cues_of(r, TC_FLING) == 1);
        run_for(r, .2f, NULL, 0);
        for (int k = 0; k < 2; k++) {
            int direction = k ? dy : dx;
            float after = r->face.tess_mood.drag_angle[k];
            if (direction) assert(before[k]*direction > .5f && (after-before[k])*direction > .1f);
            else assert(fabsf(after) < .01f);
        }
        run_for(r, 5.f, NULL, 0);
        assert(hypotf(r->face.tess_mood.drag_speed[0],r->face.tess_mood.drag_speed[1]) < .01f);
    }
}

// Taps in a row build up: the third excites it, the sixth is too much.
static void taps_build_up(void) {
    run_t *r = started();
    RUN(r, 4.f, k_taps);
    assert(cues_of(r, TC_TOUCH) == 6);
    assert(cues_of(r, TC_EXCITE) >= 3 && cues_of(r, TC_STARTLE) == 1);
    assert(r->heard[nth_cue(r, TC_EXCITE, 0)].t < r->heard[nth_cue(r, TC_STARTLE, 0)].t);
    printf("taps: 6 taps, %d excited, then startled at %.2f s\n", cues_of(r, TC_EXCITE), r->heard[nth_cue(r, TC_STARTLE, 0)].t);
}

// Left alone long enough, a touch may find it dodge (a leap away); catching it then makes it glad.
static void neglected_dodges_and_is_glad_to_be_caught(void) {
    run_t *r = started();
    play_ready_to_dodge(&r->face, k_catch, 2);
    float x0, y0, x1, y1, far = 0;
    cloud_px(&r->face, &x0, &y0);
    RUN(r, .4f, k_catch);
    for (int i = 0; i < 30; i++) {
        frame(r, k_catch, 2);
        cloud_px(&r->face, &x1, &y1);
        far = fmaxf(far, fabsf(x1 - x0));
    }
    int dodge = nth_cue(r, TC_DODGE, 0);
    assert(dodge >= 0 && r->heard[dodge].t < .6f);
    assert(far > 25.f);
    RUN(r, 1.5f, k_catch);
    int excite = nth_cue(r, TC_EXCITE, 0);
    printf("dodge: leapt %.0f px at %.2f s, glad (excite) at %.2f s when caught at 1.6 s\n", far, r->heard[dodge].t, excite >= 0 ? r->heard[excite].t : -1.f);
    assert(excite >= 0 && r->heard[excite].t > 1.5f && r->heard[excite].t < 1.9f);
    assert(r->face.tess_reaction[TR_JOY] > .3f || r->face.tess_hold[TR_JOY] > 0);
}

// Ignored: it asks (invite), asks again, and then is up to mischief, at times that vary; a touch after an invitation is answered with joy.
static void ignored_asks_then_plays_tricks(void) {
    run_t *r = started();
    run_for(r, 200.f, NULL, 0);
    int first = nth_cue(r, TC_INVITE, 0), second = nth_cue(r, TC_INVITE, 1), trick = nth_cue(r, TC_MISCHIEF, 0);
    assert(first >= 0 && second > first && trick > second);
    printf("ignored: invites at %.0f s and %.0f s, mischief at %.0f s (after 8 s alone already)\n", r->heard[first].t, r->heard[second].t, r->heard[trick].t);
    assert(r->heard[first].t + 8 > 24.f && r->heard[first].t + 8 < 41.f);
    assert(r->heard[second].t - r->heard[first].t > 34.f && r->heard[second].t - r->heard[first].t < 61.f);
    assert(r->heard[trick].t + 8 > 90.f && r->heard[trick].t + 8 < 170.f);

    run_t *q = started();
    q->face.tess_play.alone = 30; q->face.tess_play.invite_in = .8f;
    run_for(q, 3.5f, NULL, 0);
    assert(cues_of(q, TC_INVITE) == 1);
    RUN(q, 1.f, k_answer);
    assert(cues_of(q, TC_EXCITE) == 1 && cues_of(q, TC_INVITE) == 1 && cues_of(q, TC_DODGE) == 0);  // glad, not also away
}

// A hard shake startles: the sound and the hop come in that frame.
static void shake_startles(void) {
    run_t *r = started();
    face_event(&r->face, FEV_SHAKE, 0, 0);
    frame(r, NULL, 0);
    assert(nth_cue(r, TC_STARTLE, 0) == 0);
    assert(fabsf(r->face.tess_mood.shift_rate[1]) + fabsf(r->face.tess_mood.shift[1]) > .5f);
}

// Rubbing still escalates (its cues come with the strokes), on top of the touch's own answer.
static void rubbing_still_escalates(void) {
    static stroke_t k_rub[60];
    for (int i = 0; i < 60; i++) {
        float t0 = .5f + i * .12f;
        k_rub[i] = (stroke_t){t0, t0 + .12f, i % 2 ? 300.f : 180.f, 300, i % 2 ? 180.f : 300.f, 300};
    }
    run_t *r = started();
    RUN(r, 8.f, k_rub);
    printf("rub: touch answered, %d rub cues, stage %d, warm %.2f\n", cues_of(r, TC_RUB), r->face.rub.stage, r->face.tess_mood.warm);
    assert(nth_cue(r, TC_TOUCH, 0) >= 0 && cues_of(r, TC_RUB) >= 3);
    assert(r->face.tess_mood.warm > .2f);
}

// Its feelings have their own sounds: the heart for a loving word, joy for a happy one, a sigh for a sad one or a lost
// connection (after the app's own chime), and joy again as the rubbing reaches its higher stages.
static void feelings_are_heard(void) {
    const struct { emotion_t emotion; tess_cue_t cue; } words[] = {
        {EMO_LOVE, TC_LOVE}, {EMO_JOY, TC_JOY}, {EMO_HAPPY, TC_JOY}, {EMO_PROUD, TC_JOY}, {EMO_SAD, TC_SAD}};
    for (int i = 0; i < 5; i++) {
        run_t *r = started();
        face_set_emotion(&r->face, words[i].emotion, 2.f);
        frame(r, NULL, 0);
        assert(r->heard_n == 1 && r->heard[0].cue == words[i].cue);
    }
    run_t *r = started();
    face_set_emotion(&r->face, EMO_CONFUSED, 2.f);
    face_set_emotion(&r->face, EMO_ANGRY, 2.f);
    run_for(r, 1.f, NULL, 0);
    assert(cues_of(r, TC_LOVE) + cues_of(r, TC_JOY) + cues_of(r, TC_SAD) == 0);  // other moods stay silent
    face_set_mode(&r->face, MODE_OFFLINE);
    run_for(r, 2.f, NULL, 0);
    assert(cues_of(r, TC_SAD) == 0);  // the app's disconnect chime has the first word
    run_for(r, 1.f, NULL, 0);
    assert(cues_of(r, TC_SAD) == 0);  // fallen particles have no intact-character speech
    r = started();
    face_set_mode(&r->face, MODE_OFFLINE);
    run_for(r, 1.f, NULL, 0);
    face_set_mode(&r->face, MODE_IDLE);  // back before it sighed
    run_for(r, 4.f, NULL, 0);
    assert(cues_of(r, TC_SAD) == 0);
}

static void rubbing_is_glad_and_ends_in_love(void) {
    static stroke_t k_rub[160];
    for (int i = 0; i < 160; i++) {
        float t0 = .5f + i * .12f;
        k_rub[i] = (stroke_t){t0, t0 + .12f, i % 2 ? 300.f : 180.f, 300, i % 2 ? 180.f : 300.f, 300};
    }
    run_t *r = started();
    RUN(r, 20.f, k_rub);
    printf("rub feelings: %d joy, %d love, %d rewards, %d rub cues\n", cues_of(r, TC_JOY), cues_of(r, TC_LOVE), r->face.rub.joys, cues_of(r, TC_RUB));
    assert(r->face.rub.joys >= 1 && cues_of(r, TC_LOVE) == r->face.rub.joys);  // the heart is the reward
    assert(cues_of(r, TC_JOY) >= 1 && nth_cue(r, TC_JOY, 0) > nth_cue(r, TC_RUB, 2));  // and joy comes with the higher stages
}

static void automatic_sleep_is_quiet(void) {
    run_t *r = started();
    r->face.tess_play.cue_n = 0;
    tess_after(&r->face, .1f, AFTER_CONTENT, 0);
    face_set_emotion(&r->face, EMO_SLEEPY, -1);
    run_for(r, 70.f, NULL, 0);
    assert(r->heard_n == 0);  // no invitation, mood sigh, or delayed gesture

    face_set_emotion(&r->face, EMO_NEUTRAL, -1);
    tess_cue(&r->face, TC_CONTENT, 1.f, 0);
    assert(r->face.tess_play.cue_n == 1);
    face_set_mode(&r->face, MODE_SLEEP);
    run_for(r, 16.f, NULL, 0);
    assert(r->heard_n == 0 && r->face.tess_play.cue_n == 0);
    face_set_mode(&r->face, MODE_IDLE);
    r->face.dark = true;
    run_for(r, 45.f, NULL, 0);
    assert(r->heard_n == 0);
    r->face.dark = false;
    face_event(&r->face, FEV_WAKE, 0, 0);
    const stroke_t wake_tap[] = {{r->t + .5f, r->t + .6f, 340, 255, 340, 255}};
    RUN(r, 1.f, wake_tap);
    assert(cues_of(r, TC_TOUCH) == 1);  // interaction still sounds after wake
}

int main(void) {
    automatic_sleep_is_quiet();
    answers_within_a_frame();
    swipe_is_carried_and_swings_back();
    swipes_in_all_directions();
    taps_build_up();
    neglected_dodges_and_is_glad_to_be_caught();
    ignored_asks_then_plays_tricks();
    shake_startles();
    rubbing_still_escalates();
    feelings_are_heard();
    rubbing_is_glad_and_ends_in_love();
    printf("rigid through all of it: worst 4D distance error %.5f\n", worst_distance);
    assert(worst_distance < 1e-3f);
    puts("tess play ok");
    return 0;
}
