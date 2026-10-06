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

// The k-th point of a low-discrepancy sequence in the unit cube (2^32 over the plastic number and its square and cube):
// evenly spread however many are taken, and the same every time.
static float sequence(int k, int axis) {
    static const uint32_t step[3] = {3518319155u, 2882110345u, 2360945575u};
    return (float)(((uint32_t)(k + 1) * step[axis] + 0x80000000u) >> 8) * (1.f / 16777216);
}

// A random number in -32768..32767 of a dot, an axis and a tick of the shiver.
static int noise(int dot, int axis, int tick) {
    uint32_t x = (uint32_t)(dot * 3 + axis) * 0x9E3779B1u + (uint32_t)tick * 0x85EBCA6Bu;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return (int)(x >> 16) - 32768;
}

// Where slot `k` of the burst lands, in the units of the cloud's plane after the camera: a point of that sequence spread
// over the whole rectangle of the panel (and a depth to tell the dots apart by colour).
static void spot_of(int k, float out[3]) {
    float depth = (2 * sequence(k, 2) - 1) * SPOT_DEPTH, per_px = (4.7f - depth) * (1.f / 360);  // a pixel, in units at that depth
    out[0] = (2 * sequence(k, 0) - 1) * PANEL_HALF_PX * per_px;
    out[1] = (PANEL_CENTRE_Y_PX + (2 * sequence(k, 1) - 1) * PANEL_HALF_PX) * per_px;
    out[2] = depth;
}

// What every dot and slot is given, the same for all faces and every time: worked out once, not 30 times a second.
static struct {
    bool ready;
    float spot[TESS_N][3];
    struct { float leaves, letgo, strength, hold, rings, mass; } dot[TESS_N];  // hold: how far away the strain leaves it
} given;

static void hand_out(void) {
    for (int k = 0; k < TESS_N; k++) {
        spot_of(k, given.spot[k]);
        given.dot[k].leaves = OUT_LAG_S * sequence(k + 200, 0);
        given.dot[k].letgo = STRUGGLE_TO_S + RETURN_LAG_S * sequence(k + 200, 1);
        given.dot[k].strength = .7f + .5f * sequence(k + 200, 2);
        given.dot[k].hold = 1 - STRAIN_REACH * given.dot[k].strength;
        given.dot[k].rings = 2 * PI * (1.5f + .75f * sequence(k + 300, 0));  // swings in the settle, in radians
        given.dot[k].mass = .7f + .6f * sequence(k + 300, 1);
    }
    given.ready = true;
}

void tess_scatter_spot(int k, float out[3]) {
    if (!given.ready) hand_out();
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
    moment.tick = (int)(t * SHIVER_HZ);
    moment.blend = (int)(256 * smooth01(t * SHIVER_HZ - (float)moment.tick));
}

// How far of the way to its spot a dot is: 1 there, 0 home. The strain leaves it `hold` of the way when the return
// begins, and that is where the magnet takes it from.
static float reach(int dot) {
    float t = moment.t, letgo = given.dot[dot].letgo;
    if (t < STRUGGLE_FROM_S) return 1 - quartic(1 - clampf((t - given.dot[dot].leaves) * (1.f / OUT_S), 0, 1));
    if (t < letgo) return 1 - given.dot[dot].strength * moment.pull;
    float x = (t - letgo) * (1.f / APPROACH_S);
    if (x < 1) return given.dot[dot].hold * (1 - quartic(x));
    float y = (x - 1) * (APPROACH_S / SETTLE_S);
    if (y >= 1) return 0;
    // it hits the place and rings, the swing dying away to nothing (with no slope) at the end of the settle
    return -given.dot[dot].hold * RING_UNITS * tess_sin(given.dot[dot].rings * y) * (1 - y) * (1 - y);
}

// The dot's tremor, in the plane of the panel: a random push of its own that changes direction about 12 times a second
// (blended from one to the next, on integers), from the end of the flight to the dot's release.
static void shiver(int dot, float out[2]) {
    float size = moment.tremor, letgo = given.dot[dot].letgo;
    if (moment.t > letgo) size *= 1 - smooth01((moment.t - letgo) * (1.f / SHIVER_END_S));
    for (int axis = 0; axis < 2; axis++)
        out[axis] = size * (float)((noise(dot, axis, moment.tick) * (256 - moment.blend) + noise(dot, axis, moment.tick + 1) * moment.blend) >> 8);
}

void tess_scatter_apply(float p[3], int dot, int slot) {
    if (!given.ready) hand_out();
    float away = reach(dot), tremor[2] = {0, 0}, weight = away * given.dot[dot].mass;
    if (away == 0 && moment.t >= given.dot[dot].letgo) return;
    if (moment.t >= STRUGGLE_FROM_S && moment.t < given.dot[dot].letgo + SHIVER_END_S) shiver(dot, tremor);
    for (int axis = 0; axis < 3; axis++)
        p[axis] += (given.spot[slot][axis] - p[axis]) * away + (axis < 2 ? tremor[axis] + moment.sag[axis] * weight : 0);
}

// A burst that is under way is left to finish: its dots are mid-flight, and starting again would make them jump.
void tess_scatter_begin(face_t *f) {
    if (f->tess_hold[TR_SCATTER] > 0) return;
    f->tess_hold[TR_SCATTER] = TESS_BURST_S;
    f->tess_pulse = 1;
    if (f->tess_points_ready) tess_rematch(f, TS_SCATTER);  // every dot to the nearest spot: they spread outwards, not across
}

void tess_scatter_step(face_t *f, float dt) {
    float *hold = &f->tess_hold[TR_SCATTER], before = TESS_BURST_S - *hold;
    if (*hold <= 0) return;
    *hold = fmaxf(0, *hold - dt);
    f->tess_reaction[TR_SCATTER] = tess_scatter_weight(TESS_BURST_S - *hold);
    if (before < SNAP_AT_S && TESS_BURST_S - *hold >= SNAP_AT_S) tess_cue(f, TC_SNAP, 1.f, 0);
}
