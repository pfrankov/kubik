// Tess at rest is a cube in a cube, the inner one centred in the outer: the 4D angles rest at whole quarter turns (the
// tesseract mapped onto itself) and only a turn in progress shows the inner cube off-centre. Checked against the float
// projection at zero w-rotation, and over minutes of every mood and touch for a rest at a lopsided angle. cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../main/tess_geometry.c"

#define DT (1.f / 30)
#define QUARTER (PI / 2)

static float unit(void) { return rand() / (float)RAND_MAX; }

// Where the middle of each cube's 8 corners falls on the screen (px, as tess_draw projects), and how far apart.
static float cubes_apart(const face_t *f) {
    float points[TESS_N][3], wd[TESS_N], m[3][3], sum[2][2] = {{0}};
    tess_form(f, points, wd);
    tess_view_matrix(f, m);
    int count[2] = {0}, n = 0;
    for (int i = 0; i < 16; i++) {
        float r[3], p;
        tess_transform_point(m, points[n], r, false);
        p = 4.f / fmaxf(4.7f - r[2], 1.2f);
        int inner = wd[n] < 0;
        sum[inner][0] += r[0] * 90 * p; sum[inner][1] += r[1] * 90 * p; count[inner]++;
        int step = 1;
        for (int bit = 1; bit <= 8; bit <<= 1) if (!(i & bit)) step += 3;
        n += step;
    }
    return hypotf(sum[0][0] / count[0] - sum[1][0] / count[1], sum[0][1] / count[0] - sum[1][1] / count[1]);
}

static face_t posed(float xw, float zw, float phase, float pitch) {
    static face_t f;
    f = (face_t){0};
    tess_feel_reset(&f);  // (the neutral style: a zeroed one would put the camera at the cloud)
    f.view_q[0] = 1;
    f.tess_xw = xw; f.tess_zw = zw; f.tess_phase = phase;
    f.tess_mood.pose[TP_PITCH] = pitch;
    return f;
}

// Every quarter-turn combination of the x-w and z-w angles, from any side: concentric. The old resting .35 rad: not.
static void concentric_at_quarter_turns(void) {
    float worst = 0, lopsided = 0;
    srand(7);
    for (int k = 0; k < 400; k++) {
        face_t f = posed((k % 4) * QUARTER, (k / 4 % 4) * QUARTER, unit() * 2 * PI, (unit() - .5f) * .8f);
        worst = fmaxf(worst, cubes_apart(&f));
        f.tess_xw += .35f;
        lopsided = fmaxf(lopsided, cubes_apart(&f));
    }
    printf("concentric: cubes' centres %.2f px apart at worst on quarter turns; %.1f px at the most when .35 rad off\n", worst, lopsided);
    assert(worst < 1.5f);
    assert(lopsided > 20.f);  // the check would have caught it
}

static float off_quarter(float angle) {
    float turns = angle / QUARTER;
    return fabsf(turns - roundf(turns)) * QUARTER;
}

typedef struct { float angle[3], held_s[3], longest_s[3]; bool known; } rest_watch_t;

// A 4D angle standing still (< .1 rad/s) off a quarter turn (> .08 rad) for over .3 s is a lopsided rest.
static void watch(rest_watch_t *w, const face_t *f) {
    const tess_mood_t *m = &f->tess_mood;
    float now[3] = {f->tess_xw + .35f * f->tess_mode[TM_THINK] + m->w_turn[0], f->tess_zw + m->w_turn[1], m->yw + m->w_turn[2]};
    for (int k = 0; k < 3; k++) {
        bool still = w->known && fabsf(now[k] - w->angle[k]) < .1f * DT, lopsided = off_quarter(now[k]) > .08f;
        w->held_s[k] = still && lopsided ? w->held_s[k] + DT : 0;
        w->longest_s[k] = fmaxf(w->longest_s[k], w->held_s[k]);
        w->angle[k] = now[k];
    }
    w->known = true;
}

static void touch(face_t *f, int i) {
    static const face_event_t events[] = {FEV_TAP, FEV_PET, FEV_SHAKE, FEV_PICKUP, FEV_NOTIFY, FEV_NOT_HEARD, FEV_FAIL};
    face_event(f, events[i % 7], 200 + i % 90, 240);
}

// Five minutes of each mood, with a touch every few seconds, and a whole night idle.
static void no_lopsided_rests(void) {
    static const face_mode_t modes[] = {MODE_IDLE, MODE_LISTENING, MODE_THINKING, MODE_SPEAKING, MODE_IDLE, MODE_SLEEP, MODE_IDLE, MODE_OFFLINE, MODE_IDLE};
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    rest_watch_t watch_state = {0};
    float longest = 0;
    for (int t = 0; t < 9 * 300 * 30; t++) {
        if (t % (300 * 30) == 0) face_set_mode(&f, modes[t / (300 * 30)]);
        if (t % (7 * 30) == 0) touch(&f, t / (7 * 30));
        face_update(&f, DT);
        watch(&watch_state, &f);
    }
    for (int k = 0; k < 3; k++) longest = fmaxf(longest, watch_state.longest_s[k]);
    printf("rests: the longest a 4D angle stood still off a quarter turn: %.2f s (x-w %.2f, z-w %.2f, y-w %.2f)\n", longest,
           watch_state.longest_s[0], watch_state.longest_s[1], watch_state.longest_s[2]);
    assert(longest <= .3f);
}

int main(void) {
    concentric_at_quarter_turns();
    no_lopsided_rests();
    puts("tess rest ok");
    return 0;
}
