#include "tess_internal.h"
#include <string.h>

// ------------------------------------------------------------------ Tess
// One cloud of TESS_N points. At rest they sit on the 16 vertices and the 32 edges (3 points each) of a
// tesseract turning in 4D; modes and reactions are other shapes the SAME points flow into. Every change only
// moves spring targets, each point with its own delay and stiffness, so any interruption stays continuous and
// the cloud visibly pours from one form into the next instead of cross-fading.

#define TESS_VMAX 30.f
#define TESS_ZETA 0.72f       // slightly underdamped: shapes land with a small settle, jolts wobble once

// Parabolic sine, |error| < 0.001: no softfloat libm per point on the C6.
float tess_sin(float x) {
    float t = x * (1.f / (2 * PI));
    t -= (float)(int)(t + (t >= 0 ? 0.5f : -0.5f));  // -0.5 .. 0.5
    float y = 8.f * t - 16.f * t * fabsf(t);
    return 0.225f * (y * fabsf(y) - y) + y;
}

// The order in which points leave for a new shape (golden-ratio spread: neighbours differ).
float tess_delay(int i) {
    float d = i * 0.618034f;
    return d - (float)(int)d;
}
static float stagger(float w, int i) {
    if (w <= 0.001f) return 0;
    if (w >= 0.999f) return 1;
    return smooth01(w * 1.6f - tess_delay(i) * 0.6f);
}

// Depth of each point along the 4th axis (-1 far .. 1 near): the inner cube is small because it is far in
// 4D, so it must also look far (colour, size). Written by the target pass, read by the drawing.

// Tess is a rigid body: only turns move it. The 16 vertices are turned in 4D (x-w, z-w and y-w planes), then
// seen in perspective from a camera that orbits it (yaw, pitch, roll); its size may only change as a whole.
// A turn is written as a cosine and a sine that are made unit length, so a sum of squares never drifts.
static void unit_turn(float angle, float *c, float *s) {
    float cosine = tess_sin(angle + PI / 2), sine = tess_sin(angle), n = 1.f / sqrtf(cosine * cosine + sine * sine);
    *c = cosine * n; *s = sine * n;
}

// The 4D turns, as cosines and sines: x-w (the turn while thinking, plus .35 that only waiting has: it tilts the inner
// cube off-centre, so it fades in and out with the waiting), z-w, and y-w (the idle turns, the playful rolls). At rest
// every one of them is a multiple of a quarter turn, the tesseract mapped onto itself: the inner cube is centred in the
// outer one. Only a turn in progress shows it off-centre. Tess's peeks (tess_gaze.c) and idle turns (tess_mood.c) add
// whole quarters to the x-w, z-w and y-w planes.
void tess_turn4d_prepare_scaled(const face_t *f, float fourth, tess_turn4d_t *t) {
    if (fourth <= 0.f) {
        t->c[0] = t->c[1] = t->c[2] = 1.f;
        t->s[0] = t->s[1] = t->s[2] = 0.f;
        t->nod = false;
        return;
    }
    const tess_mood_t *m = &f->tess_mood;
    float a = f->tess_xw + .5f * m->drag_angle[0] + .35f * f->tess_mode[TM_THINK] + m->w_turn[0] + f->style.xw * (1 - f->tess_mode[TM_THINK]), b = f->tess_zw + m->w_turn[1], yw = m->yw - .5f * m->drag_angle[1] + m->w_turn[2];
    if (fourth != 1.f) {
        a = isfinite(a) ? a * fourth : 0.f;
        b = isfinite(b) ? b * fourth : 0.f;
        yw = isfinite(yw) ? yw * fourth : 0.f;
    }
    t->c[0] = cosf(a); t->s[0] = sinf(a);
    t->c[1] = cosf(b); t->s[1] = sinf(b);
    t->nod = fabsf(yw) > 1e-4f;
    t->c[2] = t->nod ? cosf(yw) : 1; t->s[2] = t->nod ? sinf(yw) : 0;
}

void tess_turn4d_prepare(const face_t *f, tess_turn4d_t *t) { tess_turn4d_prepare_scaled(f, 1.f, t); }

void tess_turn4d_apply(const tess_turn4d_t *t, float v[4]) {
    float x = v[0], y = v[1], z = v[2], w = v[3], u;
    u = x * t->c[0] - w * t->s[0];
    w = x * t->s[0] + w * t->c[0]; x = u;
    u = z * t->c[1] - w * t->s[1];
    w = z * t->s[1] + w * t->c[1]; z = u;
    if (t->nod) { u = y * t->c[2] - w * t->s[2]; w = y * t->s[2] + w * t->c[2]; y = u; }
    v[0] = x; v[1] = y; v[2] = z; v[3] = w;
}

// The vertices after the 4D turns and before any projection: (x, y, z, w) with x, y, z, w = +-1 turned. It turns
// in 4D while thinking (tess_xw, tess_zw: a double rotation, the inner cube swapping with the outer one, over
// and over); at rest it turns about the vertical, idle adds a small nod in the y-w plane now and then, and
// Tess's playful rolls (tess_yw) turn the same plane by whole quarters.
void tess_vertices4d(const face_t *f, float v[16][4]) {
    tess_turn4d_t turn;
    tess_turn4d_prepare(f, &turn);
    for (int i = 0; i < 16; i++) {
        v[i][0] = i & 1 ? 1 : -1; v[i][1] = i & 2 ? 1 : -1; v[i][2] = i & 4 ? 1 : -1; v[i][3] = i & 8 ? 1 : -1;
        tess_turn4d_apply(&turn, v[i]);
    }
}

// The camera: turned about Y (0.5 rad plus the slow turn, the pose, where it looks and the glance's quick offset)
// and tipped 0.38 rad about X so the inner cube shows, then rolled about Z. Rotations only.
void tess_camera_turns(const face_t *f, float turn[6]) {
    const float *pose = f->tess_mood.pose;
    float yaw = .5f + f->tess_phase + f->tess_mood.drag_angle[0] + pose[TP_YAW] + f->tess_look[0] * TESS_LOOK_YAW + f->tess_mood.glance[0] + f->tess_play.look[0];
    turn[0] = cosf(yaw); turn[1] = sinf(yaw);
    float cp, sp, cr, sr;
    unit_turn(pose[TP_PITCH] - f->tess_mood.drag_angle[1] + f->tess_look[1] * TESS_LOOK_PITCH + f->tess_mood.glance[1] + f->tess_play.look[1] + f->style.pitch, &cp, &sp);
    unit_turn(pose[TP_ROLL] + f->tess_mood.barrel, &cr, &sr);
    const float vxc0 = 0.92866f, vxs0 = 0.37092f;
    turn[2] = vxc0 * cp - vxs0 * sp; turn[3] = vxs0 * cp + vxc0 * sp;
    turn[4] = cr; turn[5] = sr;
}

// A point (or a direction) as the camera turns it: yaw, then the tip, then the roll.
void tess_camera_apply(const float turn[6], float v[3]) {
    float x = v[0], y = v[1], z = v[2], t;
    t = x * turn[0] + z * turn[1]; z = z * turn[0] - x * turn[1]; x = t;
    t = y * turn[2] - z * turn[3]; z = y * turn[3] + z * turn[2]; y = t;
    v[0] = x * turn[4] - y * turn[5]; v[1] = x * turn[5] + y * turn[4]; v[2] = z;
}

// The tesseract, point by point in its own order (vertices, then three points along each edge), and the
// depth of each along the 4th axis.
static void tess_form(const face_t *f, float points[TESS_N][3], float wd[TESS_N]) {
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

// Asleep, how far each drifted slot floats out.
static float tess_drift_far(int slot) { return 1.2f + 1.1f * fmodf(slot * .6180339f, 1); }

typedef struct {
    float time, heart, listen, mic, gc, gs, hc, hs, beat;
} target_effects_t;

static void shape_point(float *x, float *y, float *z, const float *target, int i, const target_effects_t *e) {
    float blend = stagger(e->listen, i);
    if (blend > .001f) {
        float ux = target[3] * e->gc - target[5] * e->gs;
        float uz = target[5] * e->gc + target[3] * e->gs, uy = target[4];
        float radius = 1.32f + e->mic * .22f * tess_sin(uy * 4.f - e->time * 8.f);
        *x += (ux * radius - *x) * blend; *y += (uy * radius - *y) * blend; *z += (uz * radius - *z) * blend;
    }
}

static void heart_point(float *x, float *y, float *z, const float *target, int i, const target_effects_t *e) {
    float blend = stagger(e->heart, i);
    if (blend > .001f) {
        float hx = (target[0] * e->hc + target[2] * e->hs) * e->beat;
        float hz = (target[2] * e->hc - target[0] * e->hs) * e->beat;
        *x += (hx - *x) * blend; *y += (target[1] * e->beat - *y) * blend; *z += (hz - *z) * blend;
    }
}

static void drift_point(float *x, float *y, float *z, const float *target, int i, int slot, float drift, float time) {
    float blend = stagger(drift, i);
    if (blend > .001f) {
        float far = tess_drift_far(slot), phase = time * .21f + i * 1.7f;
        float drift_x = target[3] * far + .14f * tess_sin(phase);
        float drift_y = target[4] * far + .14f * tess_sin(phase * 1.3f + 2.1f);
        float drift_z = target[5] * far + .14f * tess_sin(phase * .8f + 4.2f);
        *x += (drift_x - *x) * blend; *y += (drift_y - *y) * blend; *z += (drift_z - *z) * blend;
    }
}

// What acts on the cloud as a whole: a cock about the view axis, one uniform size, and a shift of the same
// amount for every point (a rigid body is only ever turned, resized as one, or carried).
typedef struct { float pc, ps, k, dx, dy; } rigid_t;

static rigid_t rigid_pose(const face_t *f, float time, float size_k) {
    const float *r = f->tess_reaction, *mw = f->tess_mode;
    float other = 1 - r[TR_HEART], sleep = mw[TM_SLEEP], drowsy = f->tess_sleep_t >= 0 ? 0 : sleep;
    float puzzled = r[TR_PUZZLED] * other, sad = r[TR_SAD] * other;
    float voice = f->tess_audio * (mw[TM_SPEAK] + (f->live_active ? mw[TM_LISTEN] : 0)) * other;
    float hop = r[TR_JOY] * other * .28f * fabsf(tess_sin(time * 6.5f));
    float lift = r[TR_RISE] * other * .3f - .14f * drowsy - .08f * mw[TM_OFFLINE] + .22f * r[TR_SHY] + f->style.lift;
    rigid_t b = {.pc = 1, .ps = 0, .k = size_k * (1 + voice * (.065f + .025f * tess_sin(time * 6.8f)))};
    if (puzzled > .001f) { b.pc = cosf(puzzled * .38f); b.ps = sinf(puzzled * .38f); }  // cocked like a head
    b.dx = f->tess_mood.shift[0];
    b.dy = (sad * .3f + lift) * b.k - hop + f->tess_mood.shift[1];
    return b;
}

static void apply_game_offset(const face_t *f, rigid_t *body) {
    if (!f->tess_games.ready) return;
    float dx = f->tess_games.offset[0], dy = f->tess_games.offset[1];
    if (isfinite(dx) && dx != 0) body->dx += clampf(dx, -1.25f, 1.25f);
    if (isfinite(dy) && dy != 0) body->dy += clampf(dy, -1.25f, 1.25f);
}

// A loved Tess swells a little with a heartbeat of its own (a whole-body size, two soft beats a turn of the clock).
static float mood_pulse(const face_t *f, float time) {
    if (f->style.pulse <= 0) return 1.f;
    float beat = tess_sin(time * 3.4f);
    return 1 + f->style.pulse * beat * beat;
}

// Targets of the points with (i & 1) == parity (-1: all). The shared tesseract is cheap; the per-point
// shaping is what costs on a C6 without an FPU, so each point is re-shaped every other frame.
void tess_point_targets(face_t *f, float out[TESS_N][3], int parity) {
    float points[TESS_N][3], wd[TESS_N];
    const float *mw = f->tess_mode;
    tess_form(f, points, wd);
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
    if (burst < TESS_BURST_S) tess_scatter_frame(f, burst);
    for (int i = parity < 0 ? 0 : parity; i < TESS_N; i += parity < 0 ? 1 : 2) {
        const float *target = tess_targets[f->tess_shape_of[i]], *home = points[f->tess_form_of[i]];
        float x = home[0], y = home[1], z = home[2];
        shape_point(&x, &y, &z, target, i, &shape);
        heart_point(&x, &y, &z, target, i, &shape);
        if (body.ps != 0) { float turn = x * body.pc - y * body.ps; y = x * body.ps + y * body.pc; x = turn; }
        drift_point(&x, &y, &z, target, i, f->tess_shape_of[i], drift, time);
        out[i][0] = x * body.k + body.dx;
        out[i][1] = y * body.k + body.dy;
        out[i][2] = z * body.k;
        if (burst < TESS_BURST_S) tess_scatter_apply(out[i], i, f->tess_scatter_of[i]);
        tess_ripple_point(f, &out[i][0], &out[i][1], &out[i][2]);
    }
}

// Exact damped spring for every point (four stiffness classes, so the cloud flows rather than moving as
// one rigid plate). Both position and velocity carry across every change of target.
void tess_springs(face_t *f, const float target[TESS_N][3], float dt) {
    float k[4][4];
    // The classes are 3 rad/s apart, so their decay and turn per frame step from one to the next by a fixed
    // factor and angle: two expf and four sinf/cosf a frame instead of twelve library calls.
    const float damped = sqrtf(1 - TESS_ZETA * TESS_ZETA);
    float e = expf(-TESS_ZETA * 13.f * dt), e_step = expf(-TESS_ZETA * 3.f * dt);
    float co = cosf(13.f * damped * dt), sn = sinf(13.f * damped * dt);
    float co_step = cosf(3.f * damped * dt), sn_step = sinf(3.f * damped * dt);
    for (int c = 0; c < 4; c++) {
        float w = 13.f + 3.f * c, si = sn / (w * damped);
        k[c][0] = e * (co + TESS_ZETA * w * si);
        k[c][1] = e * si;
        k[c][2] = -e * w * w * si;
        k[c][3] = e * (co - TESS_ZETA * w * si);
        float turned = co * co_step - sn * sn_step;
        sn = sn * co_step + co * sn_step;
        co = turned;
        e *= e_step;
    }
    for (int i = 0; i < TESS_N; i++) {
        const float *q = k[(i * 7) & 3];
        for (int axis = 0; axis < 3; axis++) {
            float d = f->tess_position[i][axis] - target[i][axis], v = f->tess_velocity[i][axis];
            f->tess_position[i][axis] = target[i][axis] + d * q[0] + v * q[1];
            f->tess_velocity[i][axis] = clampf(d * q[2] + v * q[3], -TESS_VMAX, TESS_VMAX);
        }
    }
}

// Rotation that keeps the model still in the room while the device turns: the inverse of the device's turn,
// written in model axes (x right, y down, z towards the viewer; the body's z points into the screen).
// The cloud turns by this fraction of the device's turn, the other way round (both tuned by hand on the device).
// Only the cloud's view is inverted: the fallen points roll by gravity (grav_x/y), which stays as it is.
// A share of a turn is not continuous all the way round: a half turn (180 deg) has no single axis, and a
// pose a hair past it would put the view the other way (a 144 deg jump). So the share fades out from a
// quarter turn on and is nothing at the half turn: the view is a plain function of the pose.
#define TESS_TILT_GAIN -.4f
#define TESS_TILT_FADE_FROM (PI / 4)  // half of the device's turn, at which the share starts to fade

void tess_view_matrix(const face_t *f, float m[3][3]) {
    float w = f->view_q[0], x = f->view_q[1], y = f->view_q[2], z = f->view_q[3];
    float n = w * w + x * x + y * y + z * z;
    if (n < 1e-6f) { w = 1; x = y = z = 0; n = 1; }
    {   // a fraction of the device turn, same axis (a negative gain turns it back)
        float v = sqrtf(x * x + y * y + z * z);
        if (w < 0) { w = -w; x = -x; y = -y; z = -z; }  // the short way round
        float half_turn = atan2f(v, w), fade = 1 - smooth01((half_turn - TESS_TILT_FADE_FROM) / (PI / 2 - TESS_TILT_FADE_FROM));
        float h = half_turn * TESS_TILT_GAIN * fade, sv = v > 1e-6f ? sinf(h) / v : 0;
        float scale = sqrtf(n);
        w = cosf(h) * scale; x *= sv * scale; y *= sv * scale; z *= sv * scale;
    }
    float k = 2 / n;
    float r[3][3] = {
        {1 - k * (y * y + z * z), k * (x * y - w * z), k * (x * z + w * y)},
        {k * (x * y + w * z), 1 - k * (x * x + z * z), k * (y * z - w * x)},
        {k * (x * z - w * y), k * (y * z + w * x), 1 - k * (x * x + y * y)},
    };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i][j] = (i == 2) != (j == 2) ? -r[i][j] : r[i][j];  // flip z in and out
}

void tess_transform_point(const float m[3][3], const float source[3], float target[3], bool transpose) {
    float x = source[0], y = source[1], z = source[2];
    for (int i = 0; i < 3; i++)
        target[i] = transpose ? m[0][i] * x + m[1][i] * y + m[2][i] * z
                              : m[i][0] * x + m[i][1] * y + m[i][2] * z;
}

// ---- Interchangeable points ------------------------------------------------------------------------------
// The form the cloud is heading for now (the blend weights follow it on their own clocks).
int tess_wanted_shape(const face_t *f) {
    if (f->tess_hold[TR_HEART] > 0) return TS_HEART;
    if (f->mode == MODE_LISTENING) return TS_GLOBE;
    if (f->tess_sleep_t >= .3f) return TS_DRIFT;
    return TS_FORM;
}
// Where slot `k` of a form is (roughly: without the per-frame sway), in object units.
static void tess_slot_pos(int shape, int k, const float form[TESS_N][3], float out[3]) {
    const float *t = tess_targets[k];
    float r = shape == TS_GLOBE ? 1.32f : shape == TS_DRIFT ? tess_drift_far(k) : 0;
    if (shape == TS_FORM) { out[0] = form[k][0]; out[1] = form[k][1]; out[2] = form[k][2]; }
    else if (shape == TS_SCATTER) tess_scatter_spot(k, out);
    else if (shape == TS_HEART) { out[0] = t[0]; out[1] = t[1]; out[2] = t[2]; }
    else { out[0] = t[3] * r; out[1] = t[4] * r; out[2] = t[5] * r; }
}
// Gives every point the nearest free place of the new form (greedy, in the points' own staggered order),
// on 32-bit integers with the free places in a list: no FPU and no 64-bit arithmetic in the 12 544 distances.
// The points in the order they pick their places: by tess_delay, so neighbours in space are far apart in it.
// Sorted once (8 ms without an FPU), when the character starts.
static const uint8_t *match_order(void) {
    static uint8_t order[TESS_N];
    static bool ordered;
    if (!ordered) {
        for (int i = 0; i < TESS_N; i++) order[i] = (uint8_t)i;
        for (int i = 1; i < TESS_N; i++) {
            uint8_t o = order[i];
            int j = i;
            while (j && tess_delay(order[j - 1]) > tess_delay(o)) { order[j] = order[j - 1]; j--; }
            order[j] = o;
        }
        ordered = true;
    }
    return order;
}
void tess_match_prepare(void) { match_order(); }
#define MATCH_RANGE 40.f  // units: 3 * (2 * 40 * 256)^2 stays below 2^31
void tess_rematch(face_t *f, int shape) {
    // ~3.3 KB of stack (16-bit coordinates): the display task has 7 KB and the heap no spare for statics.
    float form[TESS_N][3], wd[TESS_N];
    if (shape == TS_FORM) tess_form(f, form, wd);
    int16_t slot[TESS_N][3], at[TESS_N][3];  // 1/256 units
    for (int k = 0; k < TESS_N; k++) {
        float q[3];
        tess_slot_pos(shape, k, form, q);
        for (int a = 0; a < 3; a++) {
            float qa = fmaxf(-MATCH_RANGE, fminf(MATCH_RANGE, q[a])), pa = fmaxf(-MATCH_RANGE, fminf(MATCH_RANGE, f->tess_position[k][a]));
            slot[k][a] = (int16_t)(qa * 256);
            at[k][a] = (int16_t)(pa * 256);
        }
    }
    const uint8_t *order = match_order();
    uint8_t next_free[TESS_N + 1];  // next_free[TESS_N] is the head; TESS_N ends the list
    for (int i = 0; i <= TESS_N; i++) next_free[i] = (uint8_t)((i + 1) % (TESS_N + 1));
    uint8_t *map = shape == TS_FORM ? f->tess_form_of : shape == TS_SCATTER ? f->tess_scatter_of : f->tess_shape_of;
    for (int n = 0; n < TESS_N; n++) {
        int i = order[n], best = -1, best_before = TESS_N;
        int32_t best_d = INT32_MAX, x = at[i][0], y = at[i][1], z = at[i][2];
        for (int before = TESS_N, k = next_free[TESS_N]; k != TESS_N; before = k, k = next_free[k]) {
            int32_t dx = x - slot[k][0], dy = y - slot[k][1], dz = z - slot[k][2];
            int32_t d = dx * dx + dy * dy + dz * dz;
            if (d < best_d) { best_d = d; best = k; best_before = before; }
        }
        next_free[best_before] = next_free[best];
        map[i] = (uint8_t)best;
    }
}
