#include "tess_internal.h"
#include <string.h>

// The burst of a hard shake, in four parts, each dot on its own clock (seconds since the shake):
//   1. Thrown: every dot is flung to its own spot on the panel (a spread over the whole screen, edges and corners with a
//      margin) with an impulse, fastest at the very start and slowed by drag (a quartic: its speed only ever falls, and
//      ends at nothing). The screen's gravity bends the flight a little, and goes on bending it while the dots hang there.
//   2. Strain: held out there by a repulsion that fights a pull towards home which keeps growing. The dots shiver, each
//      its own way and harder and harder as the tension rises, and inch back three times, each tug stronger and each
//      pulled back like a stretched spring about to snap.
//   3. Magnet: the pull grows as the dots close in (the way home goes with the fourth power of the time: they fall
//      into place), each dot let go a little after the last.
//   4. Landing: it hits the place and rings: an underdamped overshoot with a swing or two, each dot at its own pitch.
// This is a dot effect, not the figure: the spread is free of the tesseract's rigidity, and it is drawn after the body's
// turn and size so it always reaches the panel's edges. Only positions are set here; the springs stay out of it
// (tess_motion.c), so the timing is exactly what is written below. Every number a dot needs is a pure function of its
// number and the time, on integers where it can be: this runs for all 112 dots each frame on a core without an FPU.

#define OUT_LAG_S .1f         // the last dot is thrown this long after the first
#define OUT_S .7f             // a dot's flight out
#define STRUGGLE_FROM_S (OUT_LAG_S + OUT_S)
#define STRUGGLE_S .6f
#define STRUGGLE_TO_S (STRUGGLE_FROM_S + STRUGGLE_S)
#define RETURN_LAG_S .2f      // the last dot is let go this long after the first
#define APPROACH_S .5f
#define SETTLE_S .4f
#define TESS_BURST_END_S (STRUGGLE_TO_S + RETURN_LAG_S + APPROACH_S + SETTLE_S)

// The tugs towards home, one after another: when in the strain each starts, how long it lasts, how far of the way home it gets.
static const struct { float from_s, length_s, reach; } k_tug[] = {{.04f, .22f, .05f}, {.24f, .2f, .08f}, {.42f, .18f, .12f}};
#define STRAIN_FROM_S .38f     // the pull that finally wins builds from here to the end of the strain
#define STRAIN_REACH .2f       // and leaves a dot this much of the way home when the return starts
#define SHIVER_UNITS .08f      // how far a dot shivers at most (at full tension), about 7 px
#define SHIVER_SLACK .3f       // and how much of that it does when the tension has hardly begun
#define SHIVER_HZ 12.f         // how often its direction changes
#define SHIVER_RAMP_S .08f
#define SHIVER_END_S .12f
#define SAG_UNITS .11f         // how far gravity bends a dot's place by the end of the strain, about 10 px
#define RING_UNITS .1f         // the first overshoot, as a part of the way the dot came
#define SNAP_AT_S 2.0f         // the magnet's snap: as most of the dots close on their places

// The panel: 480 x 480 px, the cloud's plane is 90 px per unit at the middle of its depth, and its centre sits at (240, 255).
#define PANEL_HALF_PX 224.f    // 16 px short of the edge: room for the shiver and the sag
#define PANEL_CENTRE_Y_PX -15.f  // the panel's middle as seen from the cloud's centre
#define SPOT_DEPTH .9f

// A random number in -32768..32767 of a dot, an axis and a tick of the shiver.
static int noise(int dot, int axis, int tick) {
    uint32_t x = (uint32_t)(dot * 3 + axis) * 0x9E3779B1u + (uint32_t)tick * 0x85EBCA6Bu;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return (int)(x >> 16) - 32768;
}

// What every slot and dot is given for one burst: worked out once when it begins, not 30 times a second. No two
// bursts are alike: each draws its own handful of clusters (strands and sprays, thrown the way she was going, if
// she was thrown) and a few loners, and every dot leaves at its own moment, flies for its own time along its own
// curve, and later slides under the tilt with its own friction.
static struct {
    bool ready;
    uint32_t seed;
    float spot[TESS_N][3];
    float spot_px[TESS_N][2];      // where each slot hangs (thrown, then slid), panel px from the cloud's centre
    int16_t slide_v[TESS_N][2];    // its sliding speed, px/s
    // Each dot's character, a byte each (0..255 over its range, see the accessors): RAM is scarce on the C6.
    struct { uint8_t leaves, out_s, curve, letgo, strength, rings, mass, grip; } dot[TESS_N];
} given;
#define Q(field, k) ((float)given.dot[k].field * (1.f / 255))
static float dot_leaves(int k) { return .12f * Q(leaves, k); }                 // most go at once, a few late
static float dot_out_s(int k) { return .38f + .32f * Q(out_s, k); }            // and fly for their own time
static float dot_curve(int k) { return (Q(curve, k) - .5f) * .9f; }            // along their own curve
static float dot_letgo(int k) { return STRUGGLE_TO_S + RETURN_LAG_S * Q(letgo, k); }
static float dot_strength(int k) { return .7f + .5f * Q(strength, k); }
static float dot_hold(int k) { return 1 - STRAIN_REACH * dot_strength(k); }
static float dot_rings(int k) { return 2 * PI * (1.5f + .75f * Q(rings, k)); }  // swings in the settle, in radians
static float dot_mass(int k) { return .7f + .6f * Q(mass, k); }
static float dot_grip(int k) { return .2f + .8f * Q(grip, k); }                // friction under the tilt: most slide, some stick
static uint8_t byte01(float x) { return (uint8_t)(clampf(x, 0, 1) * 255.f + .5f); }

static float rand01(uint32_t *seed) { *seed = *seed * 1664525u + 1013904223u; return (float)(*seed >> 8) * (1.f / 16777216); }
static float bell(uint32_t *seed) { return rand01(seed) + rand01(seed) + rand01(seed) - 1.5f; }  // -1.5..1.5, mostly near 0

// The dots stay on the square glass, 16 px from its edges. Returns which walls it met (bit 0 x, bit 1 y).
static unsigned keep_inside(float p[2]) {
    unsigned met = 0;
    if (fabsf(p[0]) > PANEL_HALF_PX) { p[0] = copysignf(PANEL_HALF_PX, p[0]); met |= 1; }
    float y = p[1] - PANEL_CENTRE_Y_PX;
    if (fabsf(y) > PANEL_HALF_PX) { p[1] = PANEL_CENTRE_Y_PX + copysignf(PANEL_HALF_PX, y); met |= 2; }
    return met;
}
// An organic spread: every dot tries a few random places and takes the one farthest from the dots already
// there (best-candidate sampling). That leaves no clumps, no dots on top of each other and no big holes, yet
// nothing lines up: no grid. How many tries each burst gets (5..8) makes some bursts looser than others; a
// throw leans the tries its way; a last small jitter keeps neighbours from looking spaced by rule. Whole pixels
// and integer distances: some 50 000 comparisons stay a couple of milliseconds on the C6, once a burst.
#define TRY_MIN 5
#define CELL_PX 60             // the neighbourhood grid: a candidate only meets the dots in its own and the 8 cells round it
#define CELLS 8
#define CELL_CAP 12
typedef struct { uint8_t n[CELLS * CELLS], dot[CELLS * CELLS][CELL_CAP]; int16_t at[TESS_N][2]; } spread_t;
static int cell_of(int x, int y) {  // x, y: whole px from the panel's top-left
    int cx = x / CELL_PX, cy = y / CELL_PX;
    return (cx < 0 ? 0 : cx >= CELLS ? CELLS - 1 : cx) + CELLS * (cy < 0 ? 0 : cy >= CELLS ? CELLS - 1 : cy);
}
// The nearest placed dot to (x, y), squared px, looking only next door; no neighbour at all counts as far.
static int32_t nearest2(const spread_t *g, int x, int y, int32_t enough) {
    int32_t nearest = 4 * CELL_PX * CELL_PX;
    int cx = cell_of(x, y) % CELLS, cy = cell_of(x, y) / CELLS;
    for (int oy = cy - 1; oy <= cy + 1; oy++)
        for (int ox = cx - 1; ox <= cx + 1; ox++) {
            if (ox < 0 || oy < 0 || ox >= CELLS || oy >= CELLS) continue;
            int c = ox + CELLS * oy;
            for (int i = 0; i < g->n[c]; i++) {
                int32_t dx = x - g->at[g->dot[c][i]][0], dy = y - g->at[g->dot[c][i]][1], d2 = dx * dx + dy * dy;
                if (d2 < nearest) nearest = d2;
                if (nearest <= enough) return nearest;  // it cannot beat the best candidate any more
            }
        }
    return nearest;
}
static int rand_px(uint32_t *seed, int span) { *seed = *seed * 1664525u + 1013904223u; return (int)((*seed >> 8) % (uint32_t)span); }
// The next burst's spread is laid out ahead of time, a few dots a frame while she is whole (tess_scatter_step),
// so a burst never stalls a frame laying out 112 dots; a burst that comes first finishes the rest at once.
#define PREPARE_PER_FRAME 16
static struct { spread_t g; uint32_t seed; int placed, tries; } s_next;
static void prepare_some(int count) {
    spread_t *g = &s_next.g;
    if (!s_next.placed && !s_next.tries) {
        memset(g->n, 0, sizeof g->n);
        s_next.seed = s_next.seed * 1664525u + 1013904223u + 0x9E3779B9u;
        s_next.tries = TRY_MIN + rand_px(&s_next.seed, 4);
    }
    int left = 240 - (int)PANEL_HALF_PX, top = 255 + (int)PANEL_CENTRE_Y_PX - (int)PANEL_HALF_PX, side = 2 * (int)PANEL_HALF_PX + 1;
    for (; count > 0 && s_next.placed < TESS_N; count--) {
        int32_t best = -1;
        int bx = 0, by = 0;
        for (int c = 0; c < s_next.tries; c++) {
            int x = left + rand_px(&s_next.seed, side), y = top + rand_px(&s_next.seed, side);
            int32_t d2 = nearest2(g, x, y, best);
            if (d2 > best) { best = d2; bx = x; by = y; }
        }
        int k = s_next.placed++, cell = cell_of(bx, by);
        g->at[k][0] = (int16_t)bx; g->at[k][1] = (int16_t)by;
        if (g->n[cell] < CELL_CAP) g->dot[cell][g->n[cell]++] = (uint8_t)k;
    }
}
static void tess_scatter_prepare(void) { if (s_next.placed < TESS_N) prepare_some(PREPARE_PER_FRAME); }
// A Tess starting afresh starts the chain of spreads afresh: a burst never depends on another face's play.
void tess_scatter_reset(void) { s_next.placed = s_next.tries = 0; s_next.seed = 0; }
// Take the prepared spread for this burst (a throw leans it its way), and start preparing the next.
static void lay_out(uint32_t *seed, float throw_x, float throw_y) {
    if (!s_next.placed) s_next.seed ^= *seed;  // (never prepared yet: draw from this burst's own seed)
    prepare_some(TESS_N);
    for (int k = 0; k < TESS_N; k++) {
        float *p = given.spot_px[k], lean = 22.f + 8.f * rand01(seed), x = (float)(s_next.g.at[k][0] - 240), y = (float)(s_next.g.at[k][1] - 255);
        // leaning out, a dot moves less the nearer the wall it already is: the throw packs, never lines dots up on the edge
        float room_x = throw_x * x > 0 ? 1 - fabsf(x) / PANEL_HALF_PX : 1, room_y = throw_y * (y - PANEL_CENTRE_Y_PX) > 0 ? 1 - fabsf(y - PANEL_CENTRE_Y_PX) / PANEL_HALF_PX : 1;
        p[0] = x + throw_x * lean * room_x + 3.f * bell(seed);
        p[1] = y + throw_y * lean * room_y + 3.f * bell(seed);
        keep_inside(p);
    }
    s_next.seed ^= *seed;
    s_next.placed = s_next.tries = 0;
}
static void hand_out(uint32_t seed, float throw_x, float throw_y) {
    given.seed = seed ? seed : 1;
    uint32_t r = given.seed;
    lay_out(&r, throw_x, throw_y);
    for (int k = 0; k < TESS_N; k++) {
        given.spot[k][2] = (2 * rand01(&r) - 1) * SPOT_DEPTH;
        given.slide_v[k][0] = given.slide_v[k][1] = 0;
        given.dot[k].leaves = byte01(rand01(&r) * rand01(&r));
        given.dot[k].out_s = byte01(rand01(&r));
        given.dot[k].curve = byte01(rand01(&r));
        given.dot[k].letgo = byte01(rand01(&r));
        given.dot[k].strength = byte01(rand01(&r));
        given.dot[k].rings = byte01(rand01(&r));
        given.dot[k].mass = byte01(rand01(&r));
        given.dot[k].grip = byte01(rand01(&r) * rand01(&r));
    }
    given.ready = true;
}
static void place_spot(int k) {
    float per_px = (4.7f - given.spot[k][2]) * (1.f / 360);
    given.spot[k][0] = given.spot_px[k][0] * per_px;
    given.spot[k][1] = given.spot_px[k][1] * per_px;
}
static void ready_spots(void) {
    if (given.ready) return;
    hand_out(0x9E3779B9u, 0, 0);
    for (int k = 0; k < TESS_N; k++) place_spot(k);
}

void tess_scatter_spot_px(int k, float *x, float *y) {
    ready_spots();
    *x = 240.f + given.spot_px[k][0];
    *y = 255.f + given.spot_px[k][1];
}

void tess_scatter_spot(int k, float out[3]) {
    ready_spots();
    memcpy(out, given.spot[k], sizeof given.spot[k]);
}

// How much of the panel the cloud is spread over (0 formed, 1 blown apart), for what the whole cloud does: the
// depth colours give way to the plain nearness, the halos are dropped (the panel repaints far less that way while the
// dots fly), and it is no tesseract until the burst is over. It lets go once the dots are closing in.
float tess_scatter_weight(float t) {
    if (t >= TESS_BURST_END_S) return 0;
    if (t < STRUGGLE_TO_S + APPROACH_S) return smooth01(t / .2f);
    return 1 - smooth01((t - STRUGGLE_TO_S - APPROACH_S) / (TESS_BURST_END_S - STRUGGLE_TO_S - APPROACH_S));
}

static float quartic(float x) { float square = x * x; return square * square; }  // slow at first, then faster and faster

// A tug towards home: rises slowly (the pull builds), then is pushed back at once. 0 before x = 0 and after x = 1.
static float tug_shape(float x) {
    if (x <= 0 || x >= 1) return 0;
    if (x < .7f) return x * x * (1.f / .49f);
    return 1 - smooth01((x - .7f) * (1.f / .3f));
}

// What is the same for every dot at one moment (a dot is a few multiplications on a core with no FPU, so none of this is
// repeated for 112 of them).
static struct {
    float t;
    float pull;        // the strain's pull towards home, 0..1 (times the dot's strength)
    float tremor;      // the shiver's size in units / 32768, before a dot's release
    float sag[2];      // gravity's bend of the flight, in units (times the dot's mass)
    int tick, blend;   // the shiver's random numbers change every tick; blend (0..256) is how far it is to the next
    const face_t *face;  // for Gather's swept slots
    bool finger;         // a Gather finger on the glass: hanging dots lean towards it
    float finger_px[2];
} moment;

void tess_scatter_frame(const face_t *f, float t) {
    moment.t = t;
    float into = t - STRUGGLE_FROM_S, strain = clampf((into - STRAIN_FROM_S) * (1.f / (STRUGGLE_S - STRAIN_FROM_S)), 0, 1);
    moment.pull = STRAIN_REACH * strain * strain;
    for (unsigned i = 0; i < sizeof k_tug / sizeof k_tug[0]; i++)
        moment.pull += k_tug[i].reach * tug_shape((into - k_tug[i].from_s) * (1.f / k_tug[i].length_s));
    float tension = smooth01(into * (1.f / STRUGGLE_S));
    moment.tremor = SHIVER_UNITS * (SHIVER_SLACK + (1 - SHIVER_SLACK) * tension * tension) * smooth01(into * (1.f / SHIVER_RAMP_S)) * (1.f / 32768);
    float gx = f->grav_x, gy = f->grav_y, thrown = clampf(t * (1.f / STRUGGLE_TO_S), 0, 1);  // no sensor (the simulator): upright, as the fall
    if (gx == 0 && gy == 0) gy = 1;
    moment.sag[0] = gx * SAG_UNITS * thrown * thrown;
    moment.sag[1] = gy * SAG_UNITS * thrown * thrown;
    float clock = t * SHIVER_HZ;
    moment.face = f;
    const tess_games_t *g = &f->tess_games;
    moment.finger = g->ready && g->game == TESS_GAME_GATHER && g->phase == TESS_GAME_WAIT && g->finger_down;
    moment.finger_px[0] = g->finger_x - 240.f;  // from the cloud's centre, panel pixels
    moment.finger_px[1] = g->finger_y - 255.f;
    if (t >= TESS_HANG_S - 1e-3f && tess_games_holds_burst(f)) {  // hanging for a finger: a slow, living tremble
        moment.tremor = SHIVER_UNITS * .45f * (1.f / 32768);
        clock = (t + f->tess_games.gather_clock) * SHIVER_HZ * .5f;
    }
    moment.tick = (int)clock;
    moment.blend = (int)(256 * smooth01(clock - (float)moment.tick));
}

#define HOME_S .3f  // a swept dot's flight home

// A dot that Gather swept: from where it hung straight into its place, fast at the end, with a small ring.
static bool swept(float p[3], int slot) {
    float since = 0;
    if (!moment.face || !tess_games_gathered(moment.face, slot, &since)) return false;
    float u = since * (1.f / HOME_S);
    if (u >= 1.6f) return true;
    float away = u < 1 ? 1 - quartic(u) : -RING_UNITS * tess_sin((u - 1) * (1.f / .6f) * 2 * PI) * (1.6f - u) * (1.f / .6f);
    float dx = given.spot[slot][0] - p[0], dy = given.spot[slot][1] - p[1];
    float bend = u < 1 ? .3f * tess_sin(PI * u) * (slot & 1 ? 1.f : -1.f) : 0;  // home along a curve, half each way
    p[0] += dx * away - dy * bend;
    p[1] += dy * away + dx * bend;
    p[2] += (given.spot[slot][2] - p[2]) * away;
    return true;
}

#define MAGNET_PX 120.f
// A hanging dot near the Gather finger leans towards it, more the nearer it is: the finger has a pull.
static void lean_to_finger(float p[3], int slot, float away) {
    if (!moment.finger || away < .5f) return;
    float dx = moment.finger_px[0] - given.spot_px[slot][0], dy = moment.finger_px[1] - given.spot_px[slot][1];
    float near = 1 - (dx * dx + dy * dy) * (1.f / (MAGNET_PX * MAGNET_PX));  // no square root: a C6 has no FPU
    if (near <= 0) return;
    float k = near * near * near * .45f * (4.7f - given.spot[slot][2]) * (1.f / 360);
    p[0] += dx * k;
    p[1] += dy * k;
}

// How far of the way to its spot a dot is: 1 there, 0 home. The strain leaves it `hold` of the way when the return
// begins, and that is where the magnet takes it from.
static float reach(int dot) {
    float t = moment.t, letgo = dot_letgo(dot);
    if (t < STRUGGLE_FROM_S) return 1 - quartic(1 - clampf((t - dot_leaves(dot)) / dot_out_s(dot), 0, 1));
    if (t < letgo) return 1 - dot_strength(dot) * moment.pull;
    float x = (t - letgo) * (1.f / APPROACH_S);
    if (x < 1) return dot_hold(dot) * (1 - quartic(x));
    float y = (x - 1) * (APPROACH_S / SETTLE_S);
    if (y >= 1) return 0;
    // it hits the place and rings, the swing dying away to nothing (with no slope) at the end of the settle
    return -dot_hold(dot) * RING_UNITS * tess_sin(dot_rings(dot) * y) * (1 - y) * (1 - y);
}

// The dot's tremor, in the plane of the panel: a random push of its own that changes direction about 12 times a second
// (blended from one to the next, on integers), from the end of the flight to the dot's release.
static void shiver(int dot, float out[2]) {
    float size = moment.tremor, letgo = dot_letgo(dot);
    if (moment.t > letgo) size *= 1 - smooth01((moment.t - letgo) * (1.f / SHIVER_END_S));
    for (int axis = 0; axis < 2; axis++)
        out[axis] = size * (float)((noise(dot, axis, moment.tick) * (256 - moment.blend) + noise(dot, axis, moment.tick + 1) * moment.blend) >> 8);
}

// On its way out a dot swings off the straight line, each its own way: sprays and swirls rather than rays.
static void curve_out(float p[3], int dot, int slot, float away) {
    if (moment.t >= STRUGGLE_FROM_S || away <= 0 || away >= 1) return;
    float dx = given.spot[slot][0] - p[0], dy = given.spot[slot][1] - p[1], bend = dot_curve(dot) * tess_sin(PI * away);
    p[0] -= dy * bend;
    p[1] += dx * bend;
}
void tess_scatter_apply(float p[3], int dot, int slot) {
    ready_spots();
    if (swept(p, slot)) return;
    float away = reach(dot), tremor[2] = {0, 0}, weight = away * dot_mass(dot);
    if (away == 0 && moment.t >= dot_letgo(dot)) return;
    if (moment.t >= STRUGGLE_FROM_S && moment.t < dot_letgo(dot) + SHIVER_END_S) shiver(dot, tremor);
    for (int axis = 0; axis < 3; axis++)
        p[axis] += (given.spot[slot][axis] - p[axis]) * away + (axis < 2 ? tremor[axis] + moment.sag[axis] * weight : 0);
    curve_out(p, dot, slot, away);
    lean_to_finger(p, slot, away);
}

// A burst that is under way is left to finish: its dots are mid-flight, and starting again would make them jump.
void tess_scatter_begin(face_t *f) {
    if (f->tess_hold[TR_SCATTER] > 0) return;
    // This burst's own spread: thrown the way she was flying, if she was.
    float vx = f->tess_games.fly_v[0], vy = f->tess_games.fly_v[1], speed = sqrtf(vx * vx + vy * vy);
    hand_out((uint32_t)(frand(f) * 4294967040.f) | 1u, speed > 1 ? vx / speed : 0, speed > 1 ? vy / speed : 0);
    for (int k = 0; k < TESS_N; k++) place_spot(k);
    f->tess_hold[TR_SCATTER] = TESS_BURST_S;
    f->tess_pulse = 1;
    if (f->tess_points_ready) tess_rematch(f, TS_SCATTER);  // every dot to the nearest spot: they spread outwards, not across
}

void tess_scatter_finish(face_t *f) {
    f->tess_hold[TR_SCATTER] = 0;
    f->tess_reaction[TR_SCATTER] = 0;
}

// While the burst hangs, the tilt pulls every free dot downhill, each against its own grip: most slide, some
// stick; they bounce off the rim and pile up against it. A dot that slides through her core is home (poured).
#define SLIDE_PX_S2 900.f
#define CORE_PX 60.f           // her body catches what slides into it
// The glass's edge: a dot bounces back a little and its knock is heard, at most every 0.12 s.
static void bounce(face_t *f, int k, float *p, const float v[2], float *knock_wait) {
    unsigned met = keep_inside(p);
    if (met & 1) given.slide_v[k][0] = (int16_t)(v[0] * -.3f);
    if (met & 2) given.slide_v[k][1] = (int16_t)(v[1] * -.3f);
    float into = (met & 1 ? fabsf(v[0]) : 0) + (met & 2 ? fabsf(v[1]) : 0);
    if (into > 120.f && *knock_wait <= 0) {
        *knock_wait = .12f;
        tess_tone(f, TT_GRAIN, tess_tone_hz(f, clampf(.5f - p[1] / 480.f, 0, 1)), fminf(1, into / 500.f), .5f);
    }
}
static void slide(face_t *f, float dt) {
    static float knock_wait;
    static bool sliding;      // some dot still moved last frame
    float gx = f->tess_games.tilt[0], gy = f->tess_games.tilt[1];
    knock_wait -= dt;
    if (gx == 0 && gy == 0 && !sliding) return;  // level and at rest: nothing to move (most of a hang)
    sliding = false;
    for (int k = 0; k < TESS_N; k++) {
        float since, *p = given.spot_px[k], v[2] = {given.slide_v[k][0], given.slide_v[k][1]};
        if (tess_games_gathered(f, k, &since)) continue;
        float grip = dot_grip(k), loose = 1 - grip, drag = fmaxf(0, 1 - dt * (1.5f + 6.f * grip));  // (linear: no expf per dot)
        v[0] = clampf((v[0] + gx * SLIDE_PX_S2 * loose * dt) * drag, -3000, 3000);
        v[1] = clampf((v[1] + gy * SLIDE_PX_S2 * loose * dt) * drag, -3000, 3000);
        given.slide_v[k][0] = (int16_t)v[0]; given.slide_v[k][1] = (int16_t)v[1];
        if (fabsf(v[0]) + fabsf(v[1]) < 1.f) continue;
        sliding = true;
        p[0] += v[0] * dt; p[1] += v[1] * dt;
        bounce(f, k, p, v, &knock_wait);
        place_spot(k);
        if (p[0] * p[0] + p[1] * p[1] < CORE_PX * CORE_PX) tess_gather_pour(f, k);
    }
}

void tess_scatter_step(face_t *f, float dt) {
    float *hold = &f->tess_hold[TR_SCATTER], before = TESS_BURST_S - *hold;
    if (*hold <= 0) { tess_scatter_prepare(); return; }
    bool hang = tess_games_holds_burst(f);
    if (hang && before >= TESS_HANG_S - 1e-3f) {  // (float: the clamp below lands a hair short) the more dots are home, the more of her glow and size returns
        slide(f, dt);
        f->tess_reaction[TR_SCATTER] = tess_scatter_weight(before) * (.88f - .3f * tess_games_gathered_share(f));  // halos stay off: the panel repaint stays small
        return;
    }
    *hold = fmaxf(0, *hold - dt);
    if (hang && TESS_BURST_S - *hold > TESS_HANG_S) *hold = TESS_BURST_S - TESS_HANG_S;  // stop exactly at the hang
    f->tess_reaction[TR_SCATTER] = tess_scatter_weight(TESS_BURST_S - *hold);
    if (before < SNAP_AT_S && TESS_BURST_S - *hold >= SNAP_AT_S) tess_cue(f, TC_SNAP, 1.f, 0);
}
