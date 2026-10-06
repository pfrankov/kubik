#include "tess_internal.h"

#include <string.h>

// ---- Offline: the points fall -----------------------------------------------------------------------------
// Without a connection the cloud stops holding its form: the points drop to wherever gravity pulls (the screen's
// own "down", see input.c), bounce a little off the edges of the screen, pile up in its corners and roll about
// as the cube is turned. They live in screen space meanwhile (the view turn is baked in when they fall and taken
// back out when they rise), and gather into the form once it is back.
#define FALL_R .068f   // a point's radius, object units
#define FALL_G 50.f    // units/s^2 at 1 g: from the middle of the screen to the edge in about a third of a second
#define FALL_SUBSTEPS 3
#define FALL_SCALE 4096
#define FALL_DIAMETER ((int32_t)(2 * FALL_R * FALL_SCALE))
#define FALL_CELL ((int32_t)(2 * FALL_R * 1.8f * FALL_SCALE))  // neighbour grid: 30 x 30 cells from -3.6, all of the screen
#define FALL_ORIGIN ((int32_t)(3.6f * FALL_SCALE))
#define FALL_GRID 30
// The screen (px of the 480 panel) as the points see it: x = 240 + 90 p x, y = 255 + 90 p y, p = 4 / (4.7 - depth).
#define FALL_MARGIN_PX 9.f       // a point's glow stays on the glass
#define FALL_PX_PER_UNIT 90.f
#define FALL_CENTER_X_PX 240.f
#define FALL_CENTER_Y_PX 255.f
#define FALL_PANEL_PX 480.f
#define FALL_BOUNCE256 96        // restitution off the screen's edge (0.375), 1/256
#define FALL_FREE256 255         // 1/256 kept per substep in the air (.998)
#define FALL_ROLL256 252         // rolling in contact: .984
#define FALL_LAZY256 236         // nearly at rest in contact: .92
#define FALL_LAZY ((int64_t)(1.2f * FALL_SCALE) * (int64_t)(1.2f * FALL_SCALE))  // squared speed (units/s) under which it is "at rest"
#define FALL_BOUNCE_MIN ((int32_t)(1.f * FALL_SCALE))  // slower than 1 unit/s into the edge it just lies there
#define FALL_REST ((int64_t)(.004f * FALL_SCALE * FALL_SCALE))  // squared speed a touching point stops below


void tess_fall_turn(face_t *f, bool fall) {
    float m[3][3];
    tess_view_matrix(f, m);
    tess_projected_t dots[TESS_N];
    if (fall) tess_project_dots(f, dots);
    for (int i = 0; i < TESS_N; i++) {
        float *vec[2] = {f->tess_position[i], f->tess_velocity[i]};
        for (int k = 0; k < 2; k++) tess_transform_point(m, vec[k], vec[k], !fall);
    }
    if (fall) {
        // Bake the actual displayed centres, including perspective and conversational zoom.
        const float units_per_q3 = 4.7f / (8.f * 90.f * 4.f);
        for (int i = 0; i < TESS_N; i++) {
            f->tess_position[i][0] = (dots[i].x - 1920) * units_per_q3;
            f->tess_position[i][1] = (dots[i].y - 2040) * units_per_q3;
            f->tess_position[i][2] = 0;
        }
        f->tess_mode[TM_OFFLINE] = 1;
    }
    f->tess_rigid = 0;
    f->tess_fallen = fall;
    if (fall) {
        tess_play_reset(f);
        f->rub = (rub_t){0};
        memset(f->tess_contact_wait, 0, sizeof f->tess_contact_wait);
        f->tess_contact_cursor = 0;
    }
    for (int k = 0; k < TESS_TWINKLES; k++) f->tess_twinkle_age[k] = f->tess_twinkle_rise[k] = f->tess_twinkle_fade[k] = 0;  // none lit
    f->tess_twinkle_next = frange(f, 1.f, 2.f);  // the heap settles a little first
    f->tess_twinkle_glint_in = frange(f, 2.5f, 5.f);
}
static uint32_t isqrt_u32(uint32_t n) {  // floor(sqrt(n)), bit by bit (n < 2^28)
    uint32_t r = 0, bit = 1u << 26;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

// The step runs on integers (FALL_SCALE units; velocities in units/s): without an FPU the float version
// spent most of a frame on it. Positions and velocities are converted once on the way in and out.
typedef struct {
    int32_t q[TESS_N][2], v[TESS_N][2];  // position and velocity across the screen
    int16_t z[TESS_N], old[TESS_N][2];   // depth; the position before this substep
    int16_t wall[TESS_N][3];             // where the screen ends for it: x (both sides), y top, y bottom
    int8_t edge[TESS_N][2];              // the edge of the screen it hit this substep, per axis: which way is inward (0: none)
    uint8_t touch[TESS_N];
    int32_t impact[TESS_N];
} fall_points_t;

static void load_fall(const face_t *f, fall_points_t *s) {
    for (int i = 0; i < TESS_N; i++) {
        for (int axis = 0; axis < 2; axis++) {
            s->q[i][axis] = (int32_t)(f->tess_position[i][axis] * FALL_SCALE);
            s->v[i][axis] = (int32_t)(f->tess_velocity[i][axis] * FALL_SCALE);
        }
        s->z[i] = (int16_t)(f->tess_position[i][2] * FALL_SCALE);
        float unit_px = FALL_PX_PER_UNIT * 4.f / (4.7f - f->tess_position[i][2]);  // px of the panel per object unit at its depth
        s->wall[i][0] = (int16_t)((FALL_CENTER_X_PX - FALL_MARGIN_PX) / unit_px * FALL_SCALE);
        s->wall[i][1] = (int16_t)((FALL_MARGIN_PX - FALL_CENTER_Y_PX) / unit_px * FALL_SCALE);
        s->wall[i][2] = (int16_t)((FALL_PANEL_PX - FALL_MARGIN_PX - FALL_CENTER_Y_PX) / unit_px * FALL_SCALE);
    }
}

static void store_fall(face_t *f, const fall_points_t *s) {
    for (int i = 0; i < TESS_N; i++) {
        for (int axis = 0; axis < 2; axis++) {
            f->tess_position[i][axis] = s->q[i][axis] * (1.f / FALL_SCALE);
            f->tess_velocity[i][axis] = s->v[i][axis] * (1.f / FALL_SCALE);
        }
        f->tess_position[i][2] = s->z[i] * (1.f / FALL_SCALE);
        f->tess_velocity[i][2] = 0;
    }
}

// Gravity, and each point easing to its own depth (tess_delay spread over +-.03: nearly one plane, so the neighbour
// test, done in object units, agrees with what the eye sees and the heap piles two or three dots deep, not one).
static void integrate_fall(fall_points_t *s, int32_t pull_x, int32_t pull_y, int32_t step16, int32_t settle16) {
    for (int i = 0; i < TESS_N; i++) {
        int32_t *q = s->q[i], *v = s->v[i];
        s->old[i][0] = (int16_t)q[0]; s->old[i][1] = (int16_t)q[1];
        v[0] += pull_x; v[1] += pull_y;
        // Rounded towards zero like the float version was: the heap slides a little slower away from the middle.
        q[0] = (int32_t)(((int64_t)q[0] * 65536 + (int64_t)v[0] * step16) / 65536);
        q[1] = (int32_t)(((int64_t)q[1] * 65536 + (int64_t)v[1] * step16) / 65536);
        int32_t depth = ((int32_t)((i * 40503u) & 0xFFFF) - 32768) >> 8;  // 40503 / 65536: tess_delay
        s->z[i] += (int16_t)(((depth - s->z[i]) * settle16) >> 16);
        s->touch[i] = 0;
        s->edge[i][0] = s->edge[i][1] = 0;
    }
}

static int fall_cell(int32_t position) { return (position + FALL_ORIGIN) / FALL_CELL; }

static int fall_grid_cell(int32_t position) {
    int cell = fall_cell(position);
    return cell < 0 ? 0 : cell >= FALL_GRID ? FALL_GRID - 1 : cell;
}

static void build_fall_grid(const fall_points_t *s, int8_t head[FALL_GRID * FALL_GRID], int8_t next[TESS_N]) {
    memset(head, -1, FALL_GRID * FALL_GRID);
    for (int i = 0; i < TESS_N; i++) {
        int x = fall_grid_cell(s->q[i][0]), y = fall_grid_cell(s->q[i][1]);
        next[i] = head[y * FALL_GRID + x];
        head[y * FALL_GRID + x] = (int8_t)i;
    }
}

static void remember_impact(fall_points_t *s, int32_t impulse, int point) {
    if (impulse > s->impact[point]) s->impact[point] = impulse;
}

static void separate_pair(fall_points_t *s, int i, int j) {
    int32_t *a = s->q[i], *b = s->q[j], dx = b[0] - a[0], dy = b[1] - a[1];
    if (dx >= FALL_DIAMETER || dx <= -FALL_DIAMETER || dy >= FALL_DIAMETER || dy <= -FALL_DIAMETER) return;
    int32_t d2 = dx * dx + dy * dy;
    if (d2 >= FALL_DIAMETER * FALL_DIAMETER || d2 == 0) return;
    int32_t d = (int32_t)isqrt_u32((uint32_t)d2), share = ((FALL_DIAMETER - d) << 15) / d;  // half the overlap, /d
    int32_t px = (int32_t)((int64_t)dx * share / 65536), py = (int32_t)((int64_t)dy * share / 65536);
    a[0] -= px; a[1] -= py; b[0] += px; b[1] += py;
    s->touch[i] = s->touch[j] = 1;
    int32_t closing = (int32_t)(((int64_t)(s->v[i][0] - s->v[j][0]) * dx +
                                  (int64_t)(s->v[i][1] - s->v[j][1]) * dy) / d);
    remember_impact(s, closing, i);
    remember_impact(s, closing, j);
}

static void separate_neighbors(const int8_t head[FALL_GRID * FALL_GRID], const int8_t next[TESS_N], fall_points_t *s) {
    for (int i = 0; i < TESS_N; i++) {
        int cx = fall_cell(s->old[i][0]), cy = fall_cell(s->old[i][1]);
        for (int oy = -1; oy <= 1; oy++)
            for (int ox = -1; ox <= 1; ox++) {
                int x = cx + ox, y = cy + oy;
                if (x < 0 || y < 0 || x >= FALL_GRID || y >= FALL_GRID) continue;
                for (int j = head[y * FALL_GRID + x]; j >= 0; j = next[j])
                    if (j > i) separate_pair(s, i, j);
            }
    }
}

// The screen's four edges hold the points in (each with the room its own depth gives it).
static void confine_to_screen(fall_points_t *s) {
    for (int i = 0; i < TESS_N; i++) {
        int32_t *q = s->q[i], sideways = s->wall[i][0];
        if (q[0] > sideways || q[0] < -sideways) { s->edge[i][0] = q[0] > 0 ? -1 : 1; q[0] = -s->edge[i][0] * sideways; }
        if (q[1] < s->wall[i][1]) { q[1] = s->wall[i][1]; s->edge[i][1] = 1; }
        if (q[1] > s->wall[i][2]) { q[1] = s->wall[i][2]; s->edge[i][1] = -1; }
        if (s->edge[i][0] || s->edge[i][1]) s->touch[i] = 1;
    }
}

static int32_t bounce_speed(int32_t arrived, int32_t edge) {
    int32_t into = arrived < 0 ? -arrived : arrived;
    return into > FALL_BOUNCE_MIN ? edge * (into * FALL_BOUNCE256 / 256) : 0;
}

// The velocity is what actually happened (dq / step), less a little drag in the air, less still rolling in a heap;
// only nearly at rest does contact hold it. A point that hit the edge of the screen goes back with a fraction of
// the speed it came in with (units/s * FALL_SCALE), unless that is a mere resting touch.
static void update_fall_velocity(fall_points_t *s, int32_t steps_per_second) {
    for (int i = 0; i < TESS_N; i++) {
        int32_t *q = s->q[i], *v = s->v[i], arrived[2] = {v[0], v[1]};
        for (int axis = 0; axis < 2; axis++) v[axis] = (q[axis] - s->old[i][axis]) * steps_per_second;
        int64_t speed2 = (int64_t)v[0] * v[0] + (int64_t)v[1] * v[1];
        int32_t factor = !s->touch[i] ? FALL_FREE256 : speed2 < FALL_LAZY ? FALL_LAZY256 : FALL_ROLL256;
        for (int axis = 0; axis < 2; axis++) {
            v[axis] = v[axis] * factor / 256;
            if (s->edge[i][axis] && (int64_t)arrived[axis] * s->edge[i][axis] < 0) {
                int32_t impact = arrived[axis] < 0 ? -arrived[axis] : arrived[axis];
                remember_impact(s, impact, i);
                v[axis] = bounce_speed(arrived[axis], s->edge[i][axis]);
            }
        }
        if (s->touch[i] && !s->edge[i][0] && !s->edge[i][1] && speed2 < FALL_REST) {
            v[0] = v[1] = 0;
            q[0] = s->old[i][0]; q[1] = s->old[i][1];
        }
    }
}

// The shimmer of a heap with nothing to do: a calm starlight. A new point begins to glow every .06 to .15 s (random
// gaps from the face's own seeded generator, never periodic), each rising over .7 to 1.3 s to (nearly) white and
// fading over .9 to 1.7 s, so some twenty of the 112 are somewhere in a twinkle at any moment, out of step with each
// other. Every few seconds one is a glint: full white, the biggest, the slowest, with a four-point star, and now and
// then a brighter visual glint; resting particles remain silent.
#define TWINKLE_GAP_MIN .06f
#define TWINKLE_GAP_MAX .15f
#define TWINKLE_PEAK_MIN 200.f
#define TWINKLE_PEAK_MAX 250.f
#define GLINT_GAP_MIN 2.5f
#define GLINT_GAP_MAX 5.f

static bool twinkle_lit(const face_t *f, int k) { return f->tess_twinkle_age[k] < f->tess_twinkle_rise[k] + f->tess_twinkle_fade[k]; }

// A point that is not twinkling already (a few tries: with a fifth of them lit, the first mostly does).
static uint8_t twinkle_pick_point(face_t *f) {
    uint8_t point = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        point = (uint8_t)((int)(frand(f) * TESS_N) % TESS_N);
        bool taken = false;
        for (int k = 0; k < TESS_TWINKLES; k++) taken |= twinkle_lit(f, k) && f->tess_twinkle_point[k] == point;
        if (!taken) break;
    }
    return point;
}

static void twinkle_step(face_t *f, float dt) {
    for (int k = 0; k < TESS_TWINKLES; k++) {
        f->tess_twinkle_age[k] += dt;
    }
    f->tess_twinkle_glint_in -= dt;
    if ((f->tess_twinkle_next -= dt) > 0) return;
    f->tess_twinkle_next = frange(f, TWINKLE_GAP_MIN, TWINKLE_GAP_MAX);
    for (int k = 0; k < TESS_TWINKLES; k++) {
        if (twinkle_lit(f, k)) continue;
        bool glint = f->tess_twinkle_glint_in <= 0;
        f->tess_twinkle_age[k] = 0;
        f->tess_twinkle_rise[k] = glint ? frange(f, 1.3f, 1.6f) : frange(f, .7f, 1.3f);
        f->tess_twinkle_fade[k] = glint ? frange(f, 2.2f, 2.8f) : frange(f, .9f, 1.7f);
        f->tess_twinkle_peak[k] = glint ? TESS_GLINT_PEAK : (uint8_t)frange(f, TWINKLE_PEAK_MIN, TWINKLE_PEAK_MAX);
        f->tess_twinkle_point[k] = twinkle_pick_point(f);
        if (glint) {
            f->tess_twinkle_glint_in = frange(f, GLINT_GAP_MIN, GLINT_GAP_MAX);
        }
        return;
    }
}

// How lit slot k is, 0..1: a smoothstep up to its peak, then a smoothstep down (no linear ramp, no hard edge).
float tess_twinkle_level(const face_t *f, int k) {
    float age = f->tess_twinkle_age[k], rise = f->tess_twinkle_rise[k];
    if (!twinkle_lit(f, k)) return 0.f;
    float shape = age < rise ? smooth01(age / rise) : 1 - smooth01((age - rise) / f->tess_twinkle_fade[k]);
    return shape * f->tess_twinkle_peak[k] * (1.f / 255);
}

void tess_fall_step(face_t *f, float dt) {
    // Position-based: move, then push apart whatever overlaps (and back inside the screen), and take the
    // velocity from what actually happened. A heap then rests instead of jittering.
    if (dt <= 0) return;
    twinkle_step(f, dt);
    float gx = f->grav_x, gy = f->grav_y;
    if (gx == 0 && gy == 0) gy = 1;  // no sensor (the simulator): upright
    float h = dt / FALL_SUBSTEPS;
    int32_t pull_x = (int32_t)(gx * FALL_G * h * FALL_SCALE), pull_y = (int32_t)(gy * FALL_G * h * FALL_SCALE);
    int32_t step16 = (int32_t)(h * 65536), settle16 = (int32_t)(fminf(1, h * 4) * 65536);
    int32_t steps_per_second = (int32_t)(1.f / h);
    int8_t head[FALL_GRID * FALL_GRID];  // ~3.6 KB of stack with the points (as much as tess_rematch): runs outside drawing
    int8_t next[TESS_N];
    fall_points_t s = {0};
    load_fall(f, &s);
    for (int sub = 0; sub < FALL_SUBSTEPS; sub++) {
        integrate_fall(&s, pull_x, pull_y, step16, settle16);
        build_fall_grid(&s, head, next);
        for (int pass = 0; pass < 2; pass++) {
            separate_neighbors(head, next, &s);
            confine_to_screen(&s);
        }
        update_fall_velocity(&s, steps_per_second);
    }
    store_fall(f, &s);
    // One contact per point per frame, including both partners. Rotate the starting
    // point so a dense heap does not always give the six cue slots to the same dots.
    int emitted = 0, cursor = f->tess_contact_cursor;
    for (int i = 0; i < TESS_N; i++)
        if (f->tess_contact_wait[i]) f->tess_contact_wait[i]--;
    for (int n = 0; n < TESS_N && emitted < TESS_CUES; n++) {
        int i = (cursor + n) % TESS_N;
        if (f->tess_contact_wait[i] || s.impact[i] < (int32_t)(2.8f * FALL_SCALE)) continue;
        tess_cue(f, TC_IMPACT, clampf(s.impact[i] * (1.f / (20 * FALL_SCALE)), 0, 1),
                 i * (2.f / (TESS_N - 1)) - 1);
        f->tess_contact_wait[i] = 3;  // repeated solver corrections are not new strikes
        f->tess_contact_cursor = (uint8_t)((i + 1) % TESS_N);
        emitted++;
    }
}
