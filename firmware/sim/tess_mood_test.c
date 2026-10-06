// Tess's idle moves and reactions (tess_mood.c): the pose and its speed change smoothly frame to frame, idle turns
// both ways and rests, its 4D turns are quarter turns that rest at a symmetric angle, waiting keeps its own,
// and the sensors' moods shift it the way they should. cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../main/tess_draw.c"

#define DT (1.f / 30)
#define POSE_STEP_LIMIT .07f   // rad per frame (2.1 rad/s: a pose the attention carried 2-3 rad to face the viewer comes home at that speed)
#define RATE_STEP_LIMIT .3f    // rad/s per frame: pose speed never jumps
#define SPIN_STEP_LIMIT .04f   // rad/s per frame

static face_t fresh(void) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    return f;
}

typedef struct { float pose[TP_COUNT], rate[TP_COUNT], spin, phase; } snap_t;
static snap_t snap(const face_t *f) {
    snap_t s = {.spin = f->tess_mood.spin, .phase = f->tess_phase};
    memcpy(s.pose, f->tess_mood.pose, sizeof s.pose);
    memcpy(s.rate, f->tess_mood.rate, sizeof s.rate);
    return s;
}

// Worst frame-to-frame change so far; the pose stays finite and inside its range.
typedef struct { float pose, rate, spin, phase; } worst_t;
static void compare(worst_t *w, const snap_t *before, const snap_t *now, float dt) {
    static const float range[TP_COUNT] = {3.5f, 1.f, .45f};  // yaw and pitch: turning its face to the viewer (tess_gaze.c) takes up to a half turn about the vertical and the tip's .38
    for (int k = 0; k < TP_COUNT; k++) {
        assert(isfinite(now->pose[k]) && fabsf(now->pose[k]) <= range[k]);
        w->pose = fmaxf(w->pose, fabsf(now->pose[k] - before->pose[k]) * DT / dt);
        w->rate = fmaxf(w->rate, fabsf(now->rate[k] - before->rate[k]) * DT / dt);
    }
    w->spin = fmaxf(w->spin, fabsf(now->spin - before->spin) * DT / dt);
    w->phase = fmaxf(w->phase, fabsf(now->phase - before->phase) / dt);
}
static void assert_smooth(const worst_t *w, float pose, float rate, float spin, const char *what) {
    printf("%s: worst frame step: pose %.4f rad, pose speed %.3f rad/s, spin %.4f rad/s\n", what, w->pose, w->rate, w->spin);
    assert(w->pose <= pose && w->rate <= rate && w->spin <= spin);
}

typedef struct { float spin_lo, spin_hi, w4_peak, w4_speed, still, last_sign, goal; int reversals, turns; } idle_stats_t;
static void track_idle(idle_stats_t *s, const face_t *f, float dt) {
    float spin = f->tess_mood.spin, w4 = f->tess_mood.w_turn[2];
    s->spin_lo = fminf(s->spin_lo, spin); s->spin_hi = fmaxf(s->spin_hi, spin);
    if (fabsf(spin) < .05f) s->still += dt;
    if (fabsf(spin) > .08f) {
        float sign = spin < 0 ? -1.f : 1.f;
        if (s->last_sign && sign != s->last_sign) s->reversals++;
        s->last_sign = sign;
    }
    s->w4_peak = fmaxf(s->w4_peak, fabsf(w4));
    s->w4_speed = fmaxf(s->w4_speed, fabsf(f->tess_mood.w_rate[2]));
    if (f->tess_mood.w_goal[2] != s->goal) { s->goal = f->tess_mood.w_goal[2]; s->turns++; }  // each is a new quarter turn or two
}

// Two minutes of idle at 30 fps and again at uneven frame times: smooth, turns both ways, rests, and takes its
// turns in 4D as quarter turns (waiting's x-w and z-w stay put).
static void idle_is_smooth_and_alive(bool uneven) {
    face_t f = fresh();
    worst_t w = {0};
    idle_stats_t s = {.spin_lo = 9, .spin_hi = -9};
    unsigned seed = 5;
    float t = 0;
    snap_t before = snap(&f);
    while (t < 120) {
        seed = seed * 1664525u + 1013904223u;
        float dt = uneven ? (27 + (seed >> 16) % 14) / 1000.f : DT;
        face_update(&f, dt);
        t += dt;
        snap_t now = snap(&f);
        compare(&w, &before, &now, dt);
        before = now;
        assert(f.tess_xw == 0 && f.tess_zw == 0);
        track_idle(&s, &f, dt);
    }
    printf("idle %s: spin %.2f .. %.2f rad/s, %d reversals, resting %.0f%% of the time, %d 4D turns, peak %.2f rad at up to %.2f rad/s\n",
           uneven ? "uneven" : "30 fps", s.spin_lo, s.spin_hi, s.reversals, 100 * s.still / t, s.turns, s.w4_peak, s.w4_speed);
    assert_smooth(&w, POSE_STEP_LIMIT, RATE_STEP_LIMIT, SPIN_STEP_LIMIT, "idle");
    assert(s.spin_lo < -.1f && s.spin_hi > .1f && s.reversals >= 1);  // not one way (attentive phases hold the turn still, so there are fewer)
    assert(s.still > .05f * t);                                        // it does come to rest
    assert(s.turns >= 1 && s.turns <= 12);                             // rare (peeks through the 4th dimension included)
    assert(s.w4_peak >= 1.5f && s.w4_peak <= 4.8f && s.w4_speed < 2.f);  // quarter turns, one after another if it comes to that
}

// The dots themselves (kicks off: the moves alone): steady steps, no jumps.
static void idle_dots_are_smooth(void) {
    face_t f = fresh();
    static dot_t dots[3][TESS_N];
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, worst_step = 0, worst_change = 0;
    for (int i = 0; i < 60 * 30; i++) {
        f.tess_next_fidget = 1e9f;
        face_update(&f, DT);
        dot_t *now = dots[i % 3], *before = dots[(i + 2) % 3], *earlier = dots[(i + 1) % 3];
        project_points(&f, f.tess_position, m, 4.7f, .9f, 1.f, 2.3f, 1 / 4.6f, now);
        if (i < 90) continue;  // it unfolds from a spark first
        for (int k = 0; k < TESS_N; k++) {
            worst_step = fmaxf(worst_step, hypotf(now[k].x - before[k].x, now[k].y - before[k].y));
            worst_change = fmaxf(worst_change, hypotf((now[k].x - before[k].x) - (before[k].x - earlier[k].x),
                                                     (now[k].y - before[k].y) - (before[k].y - earlier[k].y)));
        }
    }
    printf("idle dots: biggest step %.0f, biggest change of a step %.1f (1/8 px per frame)\n", worst_step, worst_change);
    assert(worst_step <= 300.f && worst_change <= 64.f);  // (saccades and the quarter turns of a peek are quick starts on purpose: 7 px in a frame at most; a jolt is over 100)
}

// Every context in turn, events and mode changes included: nothing jumps.
static void reactions_are_smooth(void) {
    face_t f = fresh();
    worst_t w = {0};
    snap_t before = snap(&f);
    for (int i = 0; i < 40 * 30; i++) {
        float t = i * DT;
        if (i == 10 * 30) face_event(&f, FEV_TAP, 400, 250);
        if (i == 12 * 30) face_event(&f, FEV_PICKUP, 1, 0);
        if (i == 14 * 30) face_event(&f, FEV_SHAKE, 0, 0);
        if (i == 19 * 30) face_event(&f, FEV_NOTIFY, 0, 0);
        if (i == 22 * 30) face_set_mode(&f, MODE_LISTENING);
        if (i == 25 * 30) face_set_mode(&f, MODE_SPEAKING);
        if (i == 28 * 30) face_set_mode(&f, MODE_THINKING);
        if (i == 32 * 30) face_set_mode(&f, MODE_OFFLINE);
        if (i == 35 * 30) face_set_mode(&f, MODE_IDLE);
        f.mic_level = f.spk_level = .5f + .5f * sinf(t * 6);
        face_update(&f, DT);
        snap_t now = snap(&f);
        compare(&w, &before, &now, DT);
        before = now;
    }
    assert_smooth(&w, .05f, .5f, SPIN_STEP_LIMIT, "reactions");
}

// `tired`: nobody has touched it for a long time, and it has not moved.
static float mean_abs_spin(face_t *f, float seconds, bool tired, float *turn4d_s) {
    float sum = 0;
    *turn4d_s = 0;
    for (int i = 0; i < seconds * 30; i++) {
        if (tired) f->idle_t = f->tess_mood.still = 200;
        face_update(f, DT);
        sum += fabsf(f->tess_mood.spin);
        if (f->tess_mood.move == 4) *turn4d_s += DT;  // (the 4D turn move)
    }
    return sum / (seconds * 30);
}

// The moods: tired, flat battery and charging change how much it does.
static void moods_shift_the_moves(void) {
    face_t normal = fresh(), tired = fresh(), low = fresh();
    float peak;
    face_set_power(&low, true, 8, false, false);
    float awake = mean_abs_spin(&normal, 300, false, &peak);
    float sleepy = mean_abs_spin(&tired, 300, true, &peak);
    float flat = mean_abs_spin(&low, 300, false, &peak);
    printf("mean turn: awake %.3f, tired %.3f, flat battery %.3f rad/s; 4D turn moves when flat %.1f s\n", awake, sleepy, flat, peak);
    assert(sleepy < .6f * awake && flat < .75f * awake);
    assert(peak == 0);  // too flat for the 4D turns
}

// Waiting is untouched: its 4D angles advance by exactly the old rule, and the idle pose is gone within seconds.
static void waiting_is_unchanged(void) {
    face_t f = fresh();
    for (int i = 0; i < 20 * 30; i++) face_update(&f, DT);
    face_set_mode(&f, MODE_THINKING);
    float worst = 0;
    for (int i = 0; i < 15 * 30; i++) {
        float xw = f.tess_xw, zw = f.tess_zw, think = f.tess_mode[TM_THINK];
        face_update(&f, DT);
        think = f.tess_mode[TM_THINK];  // update_modes runs first
        float dx = f.tess_xw - xw, dz = f.tess_zw - zw;
        if (dx < 0) dx += 2 * PI;
        if (dz < 0) dz += 2 * PI;
        worst = fmaxf(worst, fmaxf(fabsf(dx - DT * 1.25f * fmaxf(think, .3f)), fabsf(dz - DT * .55f * fmaxf(think, .3f))));
    }
    float pose = 0;
    for (int k = 0; k < TP_COUNT; k++) pose = fmaxf(pose, fabsf(f.tess_mood.pose[k]));
    printf("waiting: 4D angles off the old rule by %.1e rad, idle pose left %.1e rad, spin %.3f rad/s\n", worst, pose, f.tess_mood.spin);
    assert(worst < 1e-4f && pose < 2e-3f && fabsf(f.tess_mood.spin - .22f) < 2e-3f);
}

// Talking: listening faces the speaker and leans in with the voice; speaking pulses with it.
static void talking_leans_with_the_voice(void) {
    face_t f = fresh();
    for (int i = 0; i < 10 * 30; i++) face_update(&f, DT);
    face_set_mode(&f, MODE_LISTENING);
    float yaw = 0, pitch = 0;
    for (int i = 0; i < 4 * 30; i++) { f.mic_level = .8f; face_update(&f, DT); }
    yaw = f.tess_mood.pose[TP_YAW]; pitch = f.tess_mood.pose[TP_PITCH];
    printf("listening: yaw %.3f, pitch %.3f rad\n", yaw, pitch);
    assert(fabsf(yaw) < .01f && pitch < -.08f);
    face_set_mode(&f, MODE_SPEAKING);
    float lo = 9, hi = -9;
    for (int i = 0; i < 6 * 30; i++) {
        f.spk_level = fmaxf(0, sinf(i * DT * 6.3f));
        face_update(&f, DT);
        if (i > 60) { lo = fminf(lo, f.tess_mood.pose[TP_PITCH]); hi = fmaxf(hi, f.tess_mood.pose[TP_PITCH]); }
    }
    printf("speaking: pitch pulses %.3f .. %.3f rad\n", lo, hi);
    assert(hi - lo > .07f);
}

// Events: a tap on the right turns its near side to the right; a lift and a notification perk it up; a shake makes it giddy,
// then it settles into a rest.
static float largest(face_t *f, float seconds, int k, float sign) {  // sign: which way; 0: either
    float peak = 0;
    for (int i = 0; i < seconds * 30; i++) {
        face_update(f, DT);
        peak = fmaxf(peak, sign ? sign * f->tess_mood.pose[k] : fabsf(f->tess_mood.pose[k]));
    }
    return peak;
}
static void events_move_it(void) {
    face_t tap = fresh(), pickup = fresh(), notify = fresh(), shake = fresh();
    face_t *all[4] = {&tap, &pickup, &notify, &shake};
    for (int i = 0; i < 4; i++) for (int j = 0; j < 12 * 30; j++) face_update(all[i], DT);
    face_event(&tap, FEV_TAP, 440, 250);
    face_event(&pickup, FEV_PICKUP, 1, 0);
    face_event(&notify, FEV_NOTIFY, 0, 0);
    face_event(&shake, FEV_SHAKE, 0, 0);
    float turn = largest(&tap, 2.5f, TP_YAW, 1), glance = largest(&pickup, 2.5f, TP_YAW, 0);
    float wiggle = largest(&notify, 3.f, TP_ROLL, 0), swing = largest(&shake, 3.f, TP_ROLL, 0);
    largest(&shake, 4.f, TP_ROLL, 0);
    printf("events: tap turns %.2f rad towards the touch, pick-up glance %.2f, notify wiggle %.2f, shake swing %.2f, then rest (spin %.3f)\n",
           turn, glance, wiggle, swing, shake.tess_mood.spin);
    assert(turn > .15f && glance > .15f && wiggle > .08f && swing > .15f);
    assert(fabsf(shake.tess_mood.spin) < .2f && shake.tess_mood.dizzy < .05f);
}

int main(void) {
    idle_is_smooth_and_alive(false);
    idle_is_smooth_and_alive(true);
    idle_dots_are_smooth();
    reactions_are_smooth();
    moods_shift_the_moves();
    waiting_is_unchanged();
    talking_leans_with_the_voice();
    events_move_it();
    puts("tess mood ok");
    return 0;
}
