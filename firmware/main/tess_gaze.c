#include "tess_internal.h"

// Attention. Tess has a face: one of the tesseract's eight cells (+-x, +-y, +-z, +-w). It looks at you when that
// cell's normal, turned by the 4D turns, then by the camera and by the device's tilt, points at the viewer; the
// camera's yaw and pitch are what it turns to get there, so it keeps looking at you as the device tilts. Attention
// comes in turns: attentive phases (mostly facing you, tiny quick saccades and a hold between them, now and then a
// peek through the 4th dimension: a quarter turn in x-w, z-w or y-w that brings another cell round to face you)
// and wandering phases (the idle moves as they were, more 4D daydreaming, a glance back every so often).
// A tremor of the desk makes it notice you at once and peer (a slow rock of the head); being picked up or tilted
// makes it track you eagerly; a touch makes it lean in. Every change is a target for a spring: no jumps.
#define ATTEND_S_LO 8.f
#define ATTEND_S_HI 16.f
#define WANDER_S_LO 5.f
#define WANDER_S_HI 11.f
#define QUIET_SPAN_S 60.f  // s it takes to wander as much as it does, from 20 s of stillness on
#define LEAD_RAD .3f       // how far ahead of the pose its target may be (the pose spring's speed is about 1.5 x this per second)
#define STUDY_S 3.6f
#define STUDY_ROLL .13f    // rad, the peering rock
#define VIBE_LO .004f      // g*s: below it is noise, above it a shake (tess_motion's agitation takes it)
#define VIBE_HI .05f
#define SACCADE_OMEGA 12.f
#define PEEK_OMEGA 3.f
#define AIM_OMEGA 9.f
#define FAR_RAD 1.f        // rad it may take (yaw and pitch) to bring its face round before another cell is tried
#define REACT_S 3.4f       // s a reaction's turn (towards a touch, at a lift) has before it turns back to face you

// A critically damped spring, exactly (steady for any step, as the frames are uneven).
static void settle(float *x, float *rate, float goal, float omega, float dt) {
    float d = *x - goal, v = *rate, k = v + omega * d, e = expf(-omega * dt);
    if (fabsf(d) < 1e-6f && fabsf(v) < 1e-6f) { *x = goal; *rate = 0; return; }
    *x = goal + (d + k * dt) * e;
    *rate = (v - omega * k * dt) * e;
}

static float wrap(float angle) { return angle - 2 * PI * floorf((angle + PI) / (2 * PI)); }

// The camera's yaw and pitch that turn `cell` to face the viewer given the tilt of the device, the way nearest to
// where they are now; the cost is how far that is (-1: it cannot now, the cell is mostly in the 4th dimension or
// straight above or below).
static float solve(const face_t *f, const tess_turn4d_t *turn, int cell, float *yaw, float *pitch) {
    const tess_mood_t *m = &f->tess_mood;
    float n[4] = {0, 0, 0, 0}, view[3][3];
    n[cell >> 1] = cell & 1 ? -1.f : 1.f;
    tess_turn4d_apply(turn, n);
    float size = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), r = hypotf(n[0], n[2]);
    if (size < .5f || r < .85f * size) return -1;  // (a cell that is mostly up or down would need the cube stood on its head)
    tess_view_matrix(f, view);
    float roll = m->pose[TP_ROLL] + m->barrel, cr = cosf(roll), sr = sinf(roll);
    float wx = view[2][0] * cr + view[2][1] * sr, wy = view[2][1] * cr - view[2][0] * sr, wz = view[2][2];  // the way the tilt sends the viewer, before the roll
    float spread = acosf(clampf(wx * size / r, -1, 1)), base = atan2f(n[2], n[0]);
    float yaw_now = .5f + f->tess_phase + m->pose[TP_YAW], pitch_now = .38f + m->pose[TP_PITCH], best = -1;
    for (int side = -1; side <= 1; side += 2) {
        float y = base + (float)side * spread;
        float ahead = n[2] * cosf(y) - n[0] * sinf(y);
        if (ahead < 0) continue;  // (a face straight to the side has two ways round; the other is upside down)
        float p = atan2f(wz, wy) - atan2f(ahead, n[1]);
        float cost = fabsf(wrap(y - yaw_now)) + fabsf(wrap(p - pitch_now));
        if (best < 0 || cost < best) { best = cost; *yaw = y; *pitch = p; }
    }
    return best;
}

// The cell that is easiest to bring round to face the viewer now.
static void pick_cell(face_t *f) {
    tess_turn4d_t turn;
    tess_turn4d_prepare(f, &turn);
    float best = -1, yaw, pitch;
    for (int cell = 0; cell < 8; cell++) {
        float cost = solve(f, &turn, cell, &yaw, &pitch);
        if (cost >= 0 && (best < 0 || cost < best)) { best = cost; f->tess_mood.cell = (uint8_t)cell; }
    }
}

// Moves the pose's targets towards the pose that faces the viewer, by `weight`.
static void aim_face(face_t *f, float dt, float weight, float lead, float to[TP_COUNT]) {
    tess_mood_t *m = &f->tess_mood;
    tess_turn4d_t turn;
    tess_turn4d_prepare(f, &turn);
    float yaw, pitch, cost = solve(f, &turn, m->cell, &yaw, &pitch);
    if (cost < 0 || cost > FAR_RAD) {  // its face went into the 4th dimension, or a long way round: another one comes round
        pick_cell(f);
        cost = solve(f, &turn, m->cell, &yaw, &pitch);
    }
    bool ok = cost >= 0;
    if (ok) {  // (else it keeps the last aim); the aim itself is eased, so a change of cell is not a jump of the target
        float goal[2] = {m->pose[TP_YAW] + clampf(wrap(yaw - (.5f + f->tess_phase + m->pose[TP_YAW])), -lead, lead),
                         m->pose[TP_PITCH] + clampf(wrap(pitch - (.38f + m->pose[TP_PITCH])), -lead, lead)};
        for (int k = 0; k < 2; k++) settle(&m->aim[k], &m->aim_rate[k], goal[k], AIM_OMEGA, dt);
    }
    to[TP_YAW] += weight * (1 - smooth01(m->react / .55f)) * (m->aim[0] - to[TP_YAW]);  // (a reaction's own turn towards the touch comes first)
    to[TP_PITCH] += weight * (m->aim[1] - to[TP_PITCH]);
}

// The plane of the 4D turn that takes the face cell out of sight: x-w for an x cell, z-w for a z cell, y-w for a y cell,
// any for a w cell.
static int face_plane(face_t *f) {
    int axis = f->tess_mood.cell >> 1;
    return axis == 0 ? 0 : axis == 2 ? 1 : axis == 1 ? 2 : (int)(frand(f) * 3) % 3;
}

// A peek: a quarter turn in that plane, one way or the other. The cell that comes round takes over as its face on its own.
void tess_gaze_peek(face_t *f) {
    int plane = face_plane(f);
    f->tess_mood.w_goal[plane] += (frand(f) < .5f ? -1.f : 1.f) * PI / 2;
}

static void set_phase(face_t *f, bool attentive) {
    tess_mood_t *m = &f->tess_mood;
    float quiet = tess_quiet(m->still, QUIET_SPAN_S);
    m->attentive = attentive;
    m->peek_in = 0;
    if (attentive) {
        m->phase_left = frange(f, ATTEND_S_LO, ATTEND_S_HI) * (1 - .6f * quiet) * f->style.attention;
        m->saccade_in = frange(f, .8f, 1.6f) / f->style.saccade;
        if (frand(f) < .5f * f->style.attention) m->peek_in = frange(f, 1.5f, 4.f);
        pick_cell(f);
        f->tess_look_to[0] = f->tess_look_to[1] = 0;  // and the camera's own glances come home
        f->tess_next_look = frange(f, 1.5f, 3.f);
    } else {
        m->phase_left = frange(f, WANDER_S_LO, WANDER_S_HI) * (1 + quiet) / f->style.attention;
        m->check_in = frange(f, 4.f, 8.f);
    }
}

// Notices something now: it attends for at least `seconds`, and gets there at `attack` per second.
static void notice(face_t *f, float seconds, float attack) {
    tess_mood_t *m = &f->tess_mood;
    if (!m->attentive) set_phase(f, true);
    m->phase_left = fmaxf(m->phase_left, seconds);
    m->attack = fmaxf(m->attack, attack);
}

void tess_gaze_notice(face_t *f, float seconds) {
    f->tess_mood.free_t = 0;
    notice(f, seconds, 4.f);
}

void tess_gaze_event(face_t *f, face_event_t event) {
    tess_mood_t *m = &f->tess_mood;
    if (f->mode != MODE_IDLE) return;
    if (event == FEV_TAP || event == FEV_PET) {  // it leans in
        m->lean = 1;
        m->react = 1;
        notice(f, 6.f, 4.f);
    } else if (event == FEV_PICKUP || event == FEV_WAKE) {
        m->eager = 1;
        m->react = 1;
        notice(f, 6.f, 4.f);
    }
}

// A tremor of the desk (a tap nearby, a cup put down, typing): a quick glance at you, then peering.
void tess_gaze_vibration(face_t *f, float jolt) {
    tess_mood_t *m = &f->tess_mood;
    if (f->mode != MODE_IDLE || f->rub.stage > 0 || m->vibe_cool > 0 || jolt < VIBE_LO || jolt > VIBE_HI) return;
    m->vibe_cool = 2.5f;
    tess_feel_event(f, ME_JOLT, .5f, 0);
    m->eager = fmaxf(m->eager, .8f);
    m->study = STUDY_S;
    notice(f, 6.f, 5.f);
}

// Micro-saccades in an attentive phase: a small quick move of the camera, then a hold until the next.
static void saccades(face_t *f, float dt, bool calm) {
    tess_mood_t *m = &f->tess_mood;
    if (calm && (m->saccade_in -= dt) <= 0) {
        bool back = frand(f) < .3f;
        m->glance_goal[0] = back ? 0 : frange(f, -.055f, .055f) * f->style.saccade;
        m->glance_goal[1] = back ? 0 : frange(f, -.035f, .035f) * f->style.saccade;
        m->saccade_in = frange(f, .6f, 2.4f) / f->style.saccade;
    } else if (!calm) {
        m->glance_goal[0] = m->glance_goal[1] = 0;
    }
    for (int k = 0; k < 2; k++) {
        settle(&m->glance[k], &m->glance_rate[k], m->glance_goal[k], SACCADE_OMEGA, dt);
    }
}

static void peeks(face_t *f, float dt) {
    tess_mood_t *m = &f->tess_mood;
    if (m->peek_in > 0 && f->mode == MODE_IDLE && (m->peek_in -= dt) <= 0) tess_gaze_peek(f);
    for (int k = 0; k < 3; k++) {
        settle(&m->w_turn[k], &m->w_rate[k], m->w_goal[k], PEEK_OMEGA, dt);
        if (fabsf(m->w_goal[k]) > 2 * PI && fabsf(m->w_turn[k] - m->w_goal[k]) < .01f) {  // whole turns are not seen
            float whole = m->w_goal[k] > 0 ? 2 * PI : -2 * PI;
            m->w_goal[k] -= whole; m->w_turn[k] -= whole;
        }
    }
}

// While it wanders, now and then a glance back at you.
static void glance_back(face_t *f, float dt) {
    tess_mood_t *m = &f->tess_mood;
    if (m->attentive || (m->check_in -= dt) > 0) return;
    m->check = frange(f, 1.6f, 2.4f);
    m->check_in = frange(f, 5.f, 9.f);
    m->attack = fmaxf(m->attack, 3.f);
}

// The phases and their timers; whether it is looking at you now (0..1).
static float attention_wanted(face_t *f, float dt) {
    tess_mood_t *m = &f->tess_mood;
    m->vibe_cool -= dt;
    m->check -= dt;
    m->react = fmaxf(0, m->react - dt / REACT_S);
    m->study = fmaxf(0, m->study - dt);
    m->free_t = fmaxf(0, m->free_t - dt);
    m->lean *= expf(-dt * .5f);
    bool moving = m->still < .35f || m->perk > .7f;
    m->eager += ((moving ? 1.f : 0.f) - m->eager) * fminf(1, dt * (moving ? 3.f : .8f));
    if (f->mode != MODE_IDLE || f->rub.stage > 0 || m->free_t > 0) return 0;
    if ((m->phase_left -= dt) <= 0) set_phase(f, !m->attentive);
    if (!m->attentive && m->eager > .3f) notice(f, 4.f, 3.f);
    glance_back(f, dt);
    return m->attentive || m->check > 0 ? 1.f - smooth01(f->tess_mode[TM_SLEEP] * 2.f) : 0.f;
}

// Called by the mood every update, after the idle moves set their targets: pulls the pose towards facing the viewer
// as far as it is attentive, stills the endless turn, and adds the peering.
void tess_gaze_step(face_t *f, float dt, float to[TP_COUNT], float *spin) {
    tess_mood_t *m = &f->tess_mood;
    float wanted = attention_wanted(f, dt);
    m->attack += (1.f - m->attack) * fminf(1, dt * 2.f);  // the quick rise of a notice settles back to an easy one
    m->attention += (wanted - m->attention) * fminf(1, dt * (wanted > m->attention ? fmaxf(1.6f, m->attack) : m->free_t > 0 ? 8.f : 1.f));
    saccades(f, dt, m->attentive && m->eager < .5f);
    peeks(f, dt);
    float a = m->attention * fminf(1.f, f->style.attention);
    if (a > .003f) {
        aim_face(f, dt, a, LEAD_RAD * (1 + .4f * m->eager) + (m->check > 0 ? LEAD_RAD : 0.f), to);
        if (m->study > 0) to[TP_ROLL] += a * STUDY_ROLL * tess_sin(2 * PI * .55f * (STUDY_S - m->study)) * tess_sin(PI * m->study / STUDY_S);
        *spin *= 1 - a;
    }
    if (a <= .003f) {  // the aim waits where the pose is
        m->aim[0] = m->pose[TP_YAW]; m->aim[1] = m->pose[TP_PITCH];
        m->aim_rate[0] = m->aim_rate[1] = 0;
    }
}
