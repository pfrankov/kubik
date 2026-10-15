// Prior target arithmetic is an independent oracle for frame-level work sharing.
#include <assert.h>
#include <stdio.h>
#include "../main/tess_geometry.c"

static void reference_form(const face_t *f, float points[TESS_N][3], float wd[TESS_N]) {
    float v[16][3], vw[16], q[16][4], turn[6];
    tess_growth_vertices(f, q);
    tess_camera_turns(f, turn);
    const float near_w = f->style.wdist, reach = 2.6f * (near_w / 3.4f);  // (the distance the 4th axis is seen from; the size at w = 0 stays)
    for (int i = 0; i < 16; i++) {
        float p = reach / (near_w - q[i][3]);
        float x = q[i][0] * p, y = q[i][1] * p, z = q[i][2] * p;
        vw[i] = clampf(q[i][3] / 1.6f, -1, 1);
        v[i][0] = x; v[i][1] = y; v[i][2] = z;
        tess_camera_apply(turn, v[i]);
    }
    int n = 0;
    for (int i = 0; i < 16; i++) {
        wd[n] = vw[i];
        memcpy(points[n++], v[i], sizeof v[i]);
        for (int bit = 1; bit <= 8; bit <<= 1) {
            if (i & bit) continue;
            float edge[3] = {v[i | bit][0] - v[i][0], v[i | bit][1] - v[i][1], v[i | bit][2] - v[i][2]};
            float edge_w = vw[i | bit] - vw[i];
            for (float k = 1; k < 4; k++, n++) {  // three points along it
                wd[n] = vw[i] + edge_w * (k + 1) * .25f;  // the depth runs a quarter ahead of the point
                for (int axis = 0; axis < 3; axis++) points[n][axis] = v[i][axis] + edge[axis] * k * .25f;
            }
        }
    }
}

// Keep the independent oracle at a call boundary. Host inlining into compare
// changes float results even when the two target bodies are identical.
static __attribute__((noinline)) void reference_targets(face_t *f, float out[TESS_N][3], int parity) {
    float points[TESS_N][3], wd[TESS_N];
    const float *mw = f->tess_mode;
    reference_form(f, points, wd);
    for (int i = 0; i < TESS_N; i++) f->tess_wdepth[i] = wd[f->tess_form_of[i]];
    const float *r = f->tess_reaction;
    float time = fmodf(f->t, 20 * PI);
    float heart = r[TR_HEART], other = 1 - heart;
    float listen = (f->live_active ? 0 : mw[TM_LISTEN]) * other, sleep = mw[TM_SLEEP];
    // Asleep the points drift slowly apart, each its own way and distance, and float there like dust;
    // waking gathers them back (tess_drift eases out over ~6 s, back in ~1.5 s).
    float drift = f->tess_drift;
    float think = mw[TM_THINK], speak = mw[TM_SPEAK];
    // Waiting approaches; the reply stays closest, then the eased mode weights return it home.
    // One uniform scale preserves the rigid tesseract and its uninterrupted point positions.
    float breathe = (1 + (.025f + .02f * sleep) * f->style.breath * tess_sin(time * 1.8f)) * (1 + .34f * think + .40f * speak)
                  * (1 + .06f * tess_sin(time * .45f) * (1 - think) * (1 - sleep));
    float drowsy = f->tess_sleep_t >= 0 ? 0 : sleep;  // dozing in idle; asleep the points drift apart instead
    float size = (1 - .16f * drowsy - .12f * r[TR_SHY] - .1f * r[TR_SAD]) * (1 + .28f * r[TR_SURPRISE] + .06f * r[TR_JOY]);
    size *= (.04f + .96f * smooth01(f->tess_assemble)) * (1 + .05f * f->tess_mood.lean) * f->style.zoom;  // leaning in is bigger
    float hc = 1, hs = 0, beat = 1;
    if (heart > .001f) {
        float angle = .25f + tess_sin(time * .6f) * .12f;
        hc = cosf(angle); hs = sinf(angle);
        float b = tess_sin(time * 5.2f);
        beat = 1 + .05f * b * fabsf(b);  // lub-dub rather than a sine swell
    }
    float burst = TESS_BURST_S - f->tess_hold[TR_SCATTER];  // seconds into a shake's burst (TESS_BURST_S: none)
    float mic = f->tess_audio * listen;
    float gc = 1, gs = 0;
    if (listen > .001f) { gc = cosf(f->tess_phase * 1.2f); gs = sinf(f->tess_phase * 1.2f); }
    target_effects_t shape = {
        .time = time, .heart = heart, .listen = listen, .mic = mic, .gc = gc, .gs = gs,
        .hc = hc, .hs = hs, .beat = beat,
    };
    rigid_t body = rigid_pose(f, time, size * breathe * mood_pulse(f, time));
    apply_game_offset(f, &body);
    tess_play_frame(f, points, body.k, body.dx, body.dy, body.pc, body.ps);
    if (burst < TESS_BURST_S) tess_scatter_frame(f, burst);
    tess_ripple_frame_t ripples[TESS_RIPPLES];
    unsigned ripple_count = tess_ripple_frame(f, ripples);
    for (int i = parity < 0 ? 0 : parity; i < TESS_N; i += parity < 0 ? 1 : 2) {
        const float *target = tess_targets[f->tess_shape_of[i]], *home = points[f->tess_form_of[i]];
        float x = home[0], y = home[1], z = home[2];
        shape_point(&x, &y, &z, target, i, &shape);
        heart_point(&x, &y, &z, target, i, &shape);
        tess_play_point(f->tess_form_of[i], &x, &y, &z);
        if (body.ps != 0) { float turn = x * body.pc - y * body.ps; y = x * body.ps + y * body.pc; x = turn; }
        drift_point(&x, &y, &z, target, i, f->tess_shape_of[i], drift, time);
        out[i][0] = x * body.k + body.dx;
        out[i][1] = y * body.k + body.dy;
        out[i][2] = z * body.k;
        if (burst < TESS_BURST_S) tess_scatter_apply(out[i], i, f->tess_scatter_of[i]);
        tess_ripple_point(ripples, ripple_count, &out[i][0], &out[i][1], &out[i][2]);
    }
}


static void exercise(face_t *f, int frame) {
    switch(frame%300) {
    case 0: face_set_mode(f,MODE_IDLE); break;
    case 60: face_set_mode(f,MODE_LISTENING); break;
    case 100: face_set_mode(f,MODE_THINKING); break;
    case 140: face_set_mode(f,MODE_SPEAKING); break;
    case 180: face_set_mode(f,MODE_IDLE); break;
    case 220: face_set_mode(f,MODE_SLEEP); break;
    case 260: face_set_mode(f,MODE_IDLE); break;
    }
    if(frame%47==0)face_event(f,FEV_TAP,240,255);
    if(frame%300==120)face_set_emotion(f,EMO_LOVE,3);
    if(frame%300==170)face_event(f,FEV_SHAKE,240,255);
    face_update(f,1.f/30);
}
static unsigned slots_checked;
static void compare(face_t *f, int parity) {
    slots_checked += TESS_N;
    face_t actual=*f, expected=*f;
    float forms[2][TESS_N][3], depth[2][TESS_N];
    reference_form(f,forms[0],depth[0]); tess_form(f,forms[1],depth[1]);
    assert(!memcmp(forms[0],forms[1],sizeof forms[0]));
    assert(!memcmp(depth[0],depth[1],sizeof depth[0]));
    float a[TESS_N][3], b[TESS_N][3];
    memset(a,0x55,sizeof a);memcpy(b,a,sizeof a);
    reference_targets(&expected,b,parity);
    tess_point_targets(&actual,a,parity);
    if (memcmp(a,b,sizeof a)) {
        for(int i=0;i<TESS_N;i++)for(int axis=0;axis<3;axis++)if(memcmp(&a[i][axis],&b[i][axis],sizeof(float)))
            fprintf(stderr,"target mismatch t=%g parity=%d point=%d axis=%d actual=%a expected=%a listen=%g heart=%g drift=%g\n",f->t,parity,i,axis,a[i][axis],b[i][axis],f->tess_mode[TM_LISTEN],f->tess_reaction[TR_HEART],f->tess_drift);
        assert(0);
    }
    assert(!memcmp(actual.tess_wdepth,expected.tess_wdepth,sizeof actual.tess_wdepth));
    assert(actual.tess_games.shaping==expected.tess_games.shaping);
}
static void boundary_cases(const face_t *source) {
    const float weights[] = {0, nextafterf(.001f, 0), .001f, nextafterf(.001f, 1), .999f, 1};
    for (unsigned effect = 0; effect < 3; effect++) {
        for (unsigned k = 0; k < sizeof weights / sizeof *weights; k++) {
            face_t f = *source;
            f.tess_mode[TM_LISTEN] = f.tess_reaction[TR_HEART] = f.tess_drift = 0;
            float *weight[] = {&f.tess_mode[TM_LISTEN], &f.tess_reaction[TR_HEART], &f.tess_drift};
            *weight[effect] = weights[k];
            for (int parity = -1; parity < 2; parity++) compare(&f, parity);
        }
    }
}
static void quarter_oracle(void) {
    uint32_t random = 71;
    for (uint32_t i = 0; i < 0x1800000u; i++) {
        float value, expected, actual;
        uint32_t bits = i | ((i & 1) << 31);
        memcpy(&value, &bits, sizeof value);
        expected = value * .25f; actual = quarter(value);
        assert(!memcmp(&actual, &expected, sizeof actual));
    }
    for (unsigned i = 0; i < 1000000; i++) {
        random = random * 1664525u + 1013904223u;
        uint32_t bits = random;
        if ((bits & 0x7f800000u) == 0x7f800000u) bits &= ~0x00800000u;
        float value, expected, actual;
        memcpy(&value, &bits, sizeof value);
        expected = value * .25f; actual = quarter(value);
        assert(!memcmp(&actual, &expected, sizeof actual));
    }
    puts("quarter: every underflow mantissa and 1000000 finite binary32 patterns exact");
}
int main(void) {
    quarter_oracle();
    static face_t f;
    face_init(&f);face_set_character(&f,CHARACTER_TESS);
    for(int frame=0;frame<1200;frame++) {
        exercise(&f,frame);
        for(int parity=-1;parity<2;parity++)compare(&f,parity);
    }
    boundary_cases(&f);
    printf("target frame cache: %u slots byte-exact against prior float-edge loop and per-point gates, all parities/modes/reactions and gate boundaries\n", slots_checked);
}
