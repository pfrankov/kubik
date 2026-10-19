// Preserve the prior scalar scatter transform as an independent arithmetic oracle.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/tess_scatter.c"
static float reference_reach(int dot) {
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
static void reference_shiver(int dot, float out[2]) {
    float size = moment.tremor, letgo = dot_letgo(dot);
    if (moment.t > letgo) size *= 1 - smooth01((moment.t - letgo) * (1.f / SHIVER_END_S));
    for (int axis = 0; axis < 2; axis++)
        out[axis] = size * (float)((noise(dot, axis, moment.tick) * (256 - moment.blend) + noise(dot, axis, moment.tick + 1) * moment.blend) >> 8);
}


void reference_tess_scatter_apply(float p[3], int dot, int slot) {
    ready_spots();
    if (swept(p, slot)) return;
    float away = reference_reach(dot), tremor[2] = {0, 0}, weight = away * dot_mass(dot);
    if (away == 0 && moment.t >= dot_letgo(dot)) return;
    if (moment.t >= STRUGGLE_FROM_S && moment.t < dot_letgo(dot) + SHIVER_END_S) reference_shiver(dot, tremor);
    for (int axis = 0; axis < 3; axis++)
        p[axis] += (given.spot[slot][axis] - p[axis]) * away + (axis < 2 ? tremor[axis] + moment.sag[axis] * weight : 0);
    curve_out(p, dot, slot, away);
    lean_to_finger(p, slot, away);
}


static void check_decoded(uint32_t seed) {
    uint32_t r = seed;
    for (int k = 0; k < TESS_N * 7; k++) rand01(&r); // lay_out: lean + two bell calls
    for (int k = 0; k < TESS_N; k++) {
        rand01(&r); // depth
        float q = (float)byte01(rand01(&r) * rand01(&r)) * (1.f / 255);
        assert(dot_leaves(k) == .12f * q);
        q = (float)byte01(rand01(&r)) * (1.f / 255); assert(dot_out_s(k) == .38f + .32f * q);
        q = (float)byte01(rand01(&r)) * (1.f / 255); assert(dot_curve(k) == (q - .5f) * .9f);
        q = (float)byte01(rand01(&r)) * (1.f / 255); assert(dot_letgo(k) == STRUGGLE_TO_S + RETURN_LAG_S * q);
        q = (float)byte01(rand01(&r)) * (1.f / 255); assert(dot_strength(k) == .7f + .5f * q);
        q = (float)byte01(rand01(&r)) * (1.f / 255); assert(dot_rings(k) == 2 * PI * (1.5f + .75f * q));
        q = (float)byte01(rand01(&r)) * (1.f / 255); assert(dot_mass(k) == .7f + .6f * q);
        q = (float)byte01(rand01(&r) * rand01(&r)) * (1.f / 255); assert(dot_grip(k) == .2f + .8f * q);
    }
}

static void every_material_value(void) {
    for (int k = 0; k < 256; k++) {
        given.dot[0].leaves = (uint8_t)k;
        given.dot[0].out_s = (uint8_t)k;
        given.dot[0].curve = (uint8_t)k;
        given.dot[0].letgo = (uint8_t)k;
        given.dot[0].strength = (uint8_t)k;
        given.dot[0].rings = (uint8_t)k;
        given.dot[0].mass = (uint8_t)k;
        given.dot[0].grip = (uint8_t)k;
        volatile float index = (float)k;
        float q = index * (1.f / 255);
        assert(dot_leaves(0) == .12f * q);
        assert(dot_out_s(0) == .38f + .32f * q);
        assert(dot_curve(0) == (q - .5f) * .9f);
        assert(dot_letgo(0) == STRUGGLE_TO_S + RETURN_LAG_S * q);
        assert(dot_strength(0) == .7f + .5f * q);
        assert(dot_rings(0) == 2 * PI * (1.5f + .75f * q));
        assert(dot_mass(0) == .7f + .6f * q);
        assert(dot_grip(0) == .2f + .8f * q);
    }
}

static void held_sky_targets(void) {
    face_t f = {0};
    float targets[TESS_N][3];
    f.tess_games.ready = true;
    f.tess_games.game = TESS_GAME_SKY;
    f.tess_games.sky = (struct tess_sky *)&f;
    for (int i = 0; i < TESS_RIPPLES; i++) f.tess_ripple[i].age = 99;
    f.tess_hold[TR_SCATTER] = TESS_BURST_S - TESS_HANG_S;
    for (int i = 0; i < TESS_N; i++) f.tess_scatter_of[i] = (uint8_t)((i * 17) % TESS_N);
    for (int frame = 0; frame < 120; frame++) {
        f.tess_games.gather_clock = frame / 30.f;
        f.grav_x = sinf(frame); f.grav_y = cosf(frame);
        assert(tess_scatter_sky_targets(&f, targets, -1));
        for (int i = 0; i < TESS_N; i++) {
            float reference[3] = {(i - 56) * .03f, (frame - 60) * .04f, .3f};
            reference_tess_scatter_apply(reference, i, f.tess_scatter_of[i]);
            for (int axis = 0; axis < 3; axis++) assert(fabsf(reference[axis] - targets[i][axis]) < 2e-6f);
        }
    }
    memset(targets, 0, sizeof targets);
    assert(tess_scatter_sky_targets(&f, targets, 1));
    for (int i = 0; i < TESS_N; i += 2) for (int axis = 0; axis < 3; axis++) assert(targets[i][axis] == 0);
    f.tess_hold[TR_SCATTER] += .02f;
    assert(!tess_scatter_sky_targets(&f, targets, -1)); // entry still uses the body
    f.tess_hold[TR_SCATTER] -= .04f;
    assert(!tess_scatter_sky_targets(&f, targets, -1)); // return still uses the body
    f.tess_hold[TR_SCATTER] += .02f;
    f.tess_ripple[0].age = 0;
    assert(!tess_scatter_sky_targets(&f, targets, -1));
    f.tess_ripple[0].age = 99;
    f.tess_games.game = TESS_GAME_GATHER;
    assert(!tess_scatter_sky_targets(&f, targets, -1));
}

int main(void) {
    every_material_value();
    face_t f = {0};
    unsigned checked = 0;
    for (unsigned seed = 1; seed <= 16; seed++) {
        hand_out(seed * 2654435761u, .3f, -.2f);
        check_decoded(seed * 2654435761u);
        for (int k = 0; k < TESS_N; k++) place_spot(k);
        for (int frame = 0; frame <= 360; frame++) {
            float time = frame / 120.f;
            tess_scatter_frame(&f, time);
            for (int dot = 0; dot < TESS_N; dot++) {
                int slot = (dot * 17 + seed) % TESS_N;
                float before[3] = {(dot - 56) * .03f, (slot - 56) * .04f, .3f}, after[3];
                memcpy(after, before, sizeof after);
                reference_tess_scatter_apply(before, dot, slot);
                tess_scatter_apply(after, dot, slot);
                assert(!memcmp(before, after, sizeof after));
                checked++;
            }
        }
    }
    held_sky_targets();
    printf("scatter cached release: %u point transforms byte-exact across all phases\n", checked);
}
