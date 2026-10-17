#include "tess_games_internal.h"
#include "face_math.h"
#include <math.h>
#include <string.h>

// Gather. A hard landing against the rim, a run of taps or a shake bursts Tess over the whole glass. Now her dots hang out there a
// moment longer, trembling towards home. A finger that lands among them sweeps every dot it passes back
// into her, each with a note a little higher; the last one home makes her glad. Left alone, or once the
// finger has gathered nothing for a while, the rest fly home by themselves as the burst always did.
#define WAIT_S 1.8f          // s the burst hangs for a finger, beyond its own hang
#define POUR_WAIT_S 8.f      // ...or while the device is held tilted, to be poured home
#define SWEEP_R 46.f         // px either side of the finger's path
#define LAST_R 96.f          // once most are home the stragglers come from farther: no hunting in corners
#define LAST_SHARE .85f
#define IDLE_S 2.2f          // s without a dot gathered ends the round
#define END_S 1.6f

void tess_gather_clear(tess_games_t *g) {
    g->gather_clock = 0;
    g->gather_start = -1;
    memset(g->gathered_at, 0, sizeof g->gathered_at);
}
static float burst_time(const face_t *f) { return TESS_BURST_S - f->tess_hold[TR_SCATTER]; }

bool tess_games_holds_burst(const face_t *f) {
    const tess_games_t *g = &f->tess_games;
    if (!g->ready || f->tess_hold[TR_SCATTER] <= 0 || !tess_games_eligible(f)) return false;
    if (g->game == TESS_GAME_SKY) return tess_sky_holds_burst(f);
    if (g->game == TESS_GAME_GATHER) return g->phase == TESS_GAME_WAIT;
    bool tilted = fabsf(g->tilt[0]) + fabsf(g->tilt[1]) > .12f;  // held tilted, she waits to be poured (a while)
    return g->game == TESS_GAME_NONE && g->gather_clock < TESS_HANG_S + (tilted ? POUR_WAIT_S : WAIT_S);
}
float tess_games_gathered(const face_t *f, int slot, float *since) {
    const tess_games_t *g = &f->tess_games;
    if (!g->ready || slot < 0 || slot >= TESS_GATHER_SLOTS || !g->gathered_at[slot]) return 0;
    *since = g->gather_clock - (float)(g->gathered_at[slot] - 1) * .01f;
    return 1;
}

float tess_games_gathered_share(const face_t *f) {
    const tess_games_t *g = &f->tess_games;
    return g->ready && g->game == TESS_GAME_GATHER ? clampf((float)g->streak / TESS_N, 0, 1) : 0;
}

void tess_gather_track(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    if (f->tess_hold[TR_SCATTER] <= 0) {
        if (g->gather_clock > 0) tess_gather_clear(g);
        if (g->game == TESS_GAME_GATHER && g->phase == TESS_GAME_WAIT) tess_games_leave(g);
        return;
    }
    if (g->gather_clock <= 0) tess_gather_clear(g);  // a new burst: every dot is out
    g->gather_clock += fmaxf(dt, 1e-3f);
}

bool tess_gather_try_start(face_t *f) {
    tess_games_t *g = &f->tess_games;
    float t = burst_time(f);
    if (g->game || f->tess_hold[TR_SCATTER] <= 0 || t < TESS_HANG_S - .3f || t > TESS_HANG_S + .01f ||
        !tess_games_eligible(f) || !tess_games_holds_burst(f)) return false;
    tess_games_begin(f, TESS_GAME_GATHER);
    g->phase = TESS_GAME_WAIT;
    g->tick_at = -1;
    for (int i = 0; i < TESS_GATHER_SLOTS; i++) g->streak += g->gathered_at[i] != 0;
    return true;
}

// The squared distance from a point to the finger's segment; `inverse` is 1 / its squared length (0 if none).
static float segment_distance2(float px, float py, const float seg[4], float inverse) {
    float dx = seg[2] - seg[0], dy = seg[3] - seg[1];
    float u = clampf(((px - seg[0]) * dx + (py - seg[1]) * dy) * inverse, 0, 1);
    float ex = px - (seg[0] + u * dx), ey = py - (seg[1] + u * dy);
    return ex * ex + ey * ey;
}
static void win(face_t *f) {
    tess_games_earn(f, BIT_GATHERED);
    tess_temper_feel(f, .2f, -1.f, -.4f, .3f);
    tess_feel_event(f, ME_GLAD, 1.f, 0);
    tess_scatter_finish(f);
    tess_kick(f, KICK_TWIRL, 1.f);
    tess_games_celebrate(f, true);
}
void tess_gather_sweep(face_t *f, float x0, float y0, float x1, float y1) {
    tess_games_t *g = &f->tess_games;
    if (g->game != TESS_GAME_GATHER || g->phase != TESS_GAME_WAIT) return;
    float reach = g->streak >= LAST_SHARE * TESS_N ? LAST_R : SWEEP_R, reach2 = reach * reach;
    const float seg[4] = {x0, y0, x1, y1};
    float length2 = (x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0), inverse = length2 > 1e-3f ? 1.f / length2 : 0;
    unsigned gathered = 0;
    for (int slot = 0; slot < TESS_N; slot++) {  // hanging dots sit at their slots' spots: no projection
        if (g->gathered_at[slot]) continue;
        float sx, sy;
        tess_scatter_spot_px(slot, &sx, &sy);
        if (segment_distance2(sx, sy, seg, inverse) > reach2) continue;
        g->gathered_at[slot] = (uint16_t)fminf(65535.f, 1 + g->gather_clock * 100.f);
        gathered++;
    }
    if (!gathered) return;
    if (g->gather_start < 0) g->gather_start = g->gather_clock;
    g->streak = (uint16_t)(g->streak + gathered);
    g->idle = 0;
    g->pulse = fmaxf(g->pulse, .35f + .05f * (float)gathered);
    // Dots flying home breathe, higher the higher up the glass, at most every 80 ms however many come.
    if (g->clock - g->tick_at >= .08f) {
        g->tick_at = g->clock;
        tess_tone(f, TT_GRAIN, tess_tone_hz(f, clampf(1.f - y1 / 480.f, 0, 1)), fminf(1, .3f + .06f * (float)gathered), (float)g->streak / TESS_N);
    }
    if (g->streak >= TESS_N) win(f);
}
// A dot the tilt slid through her core comes home on its own, as if swept; the first one starts the round.
void tess_gather_pour(face_t *f, int slot) {
    tess_games_t *g = &f->tess_games;
    if (slot < 0 || slot >= TESS_GATHER_SLOTS || g->gathered_at[slot]) return;
    if (!g->game && !tess_gather_try_start(f)) return;
    if (g->game != TESS_GAME_GATHER || g->phase != TESS_GAME_WAIT) return;
    g->gathered_at[slot] = (uint16_t)fminf(65535.f, 1 + g->gather_clock * 100.f);
    if (g->gather_start < 0) g->gather_start = g->gather_clock;
    g->streak++;
    g->idle = 0;
    g->pulse = fmaxf(g->pulse, .4f);
    if (g->clock - g->tick_at >= .08f) {
        g->tick_at = g->clock;
        tess_tone(f, TT_GRAIN, tess_tone_hz(f, .4f + .6f * (float)g->streak / TESS_N), .35f, (float)g->streak / TESS_N);
    }
    if (g->streak >= TESS_N) win(f);
}
void tess_gather_update(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    g->age += dt;
    g->clock += dt;
    bool pouring = fabsf(g->tilt[0]) + fabsf(g->tilt[1]) > .12f;  // held tilted, she is being poured: no hurry
    g->idle = pouring ? fminf(g->idle, 1.f) : g->idle + dt;
    if (g->phase == TESS_GAME_WAIT && g->idle >= IDLE_S) tess_games_leave(g);  // the rest fly home themselves
    else if (g->phase == TESS_GAME_CELEBRATE && g->clock >= END_S) tess_games_leave(g);
}
