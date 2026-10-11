#include "tess_games_internal.h"
#include "face_math.h"
#include <math.h>
#include <string.h>

// Her structure as the toy. Every play effect is a displacement of the sixteen vertices of the tesseract;
// the three dots along each edge take the blend of their two ends, so she always stays one connected body,
// and the springs that carry each dot to its target add the follow-through: wobble, overshoot, lag and arcs.
//   A pluck rings one vertex and the ring travels out shell by shell of the hypercube (the vertices one, two,
//   three and four edges away: 4, 6, 4 and 1 of them), so every path meets again at the opposite corner.
//   Spin opens her: the outer cell flowers out and the two cells twist against each other.
//   A pull drags one vertex towards the finger; its neighbours follow less the farther they are along edges.
//   A wall flattens the side that hit it. Wrestling pushes two opposite corners apart.
#define PI 3.14159265f
#define SHELL_S .12f          // a wave's step from one shell of vertices to the next (the notes stay apart)
#define RING_W 18.85f         // rad/s: three swings a second
#define RING_DECAY 3.2f
#define STRETCH_MAX 2.6f      // units a pulled vertex may leave its place by
static const float k_reach[5] = {1.f, .42f, .17f, .06f, .02f};  // how much each edge-distance follows a pull

// For every form slot (tess_form's order: a vertex, then three dots along each of its upward edges): its two
// ends and how far along. A vertex slot is its own two ends.
static struct { bool ready; uint8_t a[TESS_N], b[TESS_N], vertex_slot[16], vertex_of[TESS_N]; float t[TESS_N]; } k_slots;
static void map_slots(void) {
    memset(k_slots.vertex_of, 255, sizeof k_slots.vertex_of);
    int n = 0;
    for (int i = 0; i < 16; i++) {
        k_slots.vertex_slot[i] = (uint8_t)n;
        k_slots.vertex_of[n] = (uint8_t)i;
        k_slots.a[n] = k_slots.b[n] = (uint8_t)i; k_slots.t[n++] = 0;
        for (int bit = 1; bit <= 8; bit <<= 1) {
            if (i & bit) continue;
            for (int k = 1; k < 4; k++, n++) { k_slots.a[n] = (uint8_t)i; k_slots.b[n] = (uint8_t)(i | bit); k_slots.t[n] = k * .25f; }
        }
    }
    k_slots.ready = true;
}
int tess_play_vertex_slot(int vertex) {
    if (!k_slots.ready) map_slots();
    return k_slots.vertex_slot[vertex];
}
int tess_play_slot_vertex(int slot) {
    if (!k_slots.ready) map_slots();
    return k_slots.vertex_of[slot] == 255 ? -1 : k_slots.vertex_of[slot];
}
static int hops(int a, int b) { return __builtin_popcount((unsigned)(a ^ b) & 15u); }
static float smooth(float x) { x = clampf(x, 0, 1); return x * x * (3 - 2 * x); }

// This frame's displacement of each vertex (units, camera-aligned: x right, y down, z towards the viewer).
static float s_move[16][3];
static bool s_moving;

static void add(int v, float x, float y, float z) { s_move[v][0] += x; s_move[v][1] += y; s_move[v][2] += z; }
static void radial(const float p[3], float out[2]) {
    float r = sqrtf(p[0] * p[0] + p[1] * p[1]);
    out[0] = r > 1e-3f ? p[0] / r : 0; out[1] = r > 1e-3f ? p[1] / r : 0;
}

// Every vertex of a shell rings the same at one moment: five values a wave, not one per corner (no FPU).
static void ring_waves(const tess_games_t *g, const float (*dir)[2]) {
    for (int w = 0; w < TESS_WAVES; w++) {
        const tess_wave_t *wave = &g->wave[w];
        if (wave->amp == 0) continue;
        float shell[5], forward = wave->amp < 0 ? -.3f : .3f;  // a ring comes forward; a recoil shrinks back
        for (int h = 0; h <= 4; h++) {
            float local = wave->age - h * wave->shell_s;
            // The first swing out is the strongest: the shell visibly pops, then rings down.
            shell[h] = local < 0 ? 0 : wave->amp * (.7f + .3f * h) * tess_sin(local * wave->ring_w) * expf(-local * RING_DECAY);
        }
        for (int v = 0; v < 16; v++) {
            float a = shell[hops(v, wave->origin)];
            if (a != 0) add(v, dir[v][0] * .55f * a, dir[v][1] * .55f * a, forward * fabsf(a));
        }
    }
}
static void open_bloom(const tess_games_t *g, const float (*home)[3]) {
    float bloom = g->bloom;
    if (bloom <= .001f) return;
    float c[2], s[2];  // the two cells twist against each other: one turn each
    for (int outer = 0; outer < 2; outer++) {
        float turn = (outer ? .55f : -.55f) * bloom;
        c[outer] = cosf(turn); s[outer] = sinf(turn);
    }
    for (int v = 0; v < 16; v++) {
        int outer = (v & 8) != 0;
        float x = home[v][0], y = home[v][1], grow = bloom * (outer ? 1.1f : .3f);
        add(v, x * c[outer] - y * s[outer] - x + x * grow, x * s[outer] + y * c[outer] - y + y * grow, (outer ? .3f : -.2f) * bloom);
    }
}
static void pull_vertex(const tess_games_t *g, const float (*home)[3], float k, float bx, float by, float pc, float ps) {
    if (g->pull_k <= .001f) return;
    float px = (g->pull[0] - GAME_CX) / (TESS_PX_PER_UNIT * k) - bx / k, py = (g->pull[1] - GAME_CY) / (TESS_PX_PER_UNIT * k) - by / k;
    float local_x = px * pc + py * ps;
    py = -px * ps + py * pc; px = local_x;
    float dx = px - home[g->grab][0], dy = py - home[g->grab][1], d = sqrtf(dx * dx + dy * dy), limit = STRETCH_MAX / k;
    if (d > limit) { dx *= limit / d; dy *= limit / d; }
    // Scared, she shies away from the finger instead of reaching for it; fond, all of her follows more eagerly.
    if (g->fear > .5f) { dx *= -.35f; dy *= -.35f; }
    float eager = 1.f + .6f * g->fond;
    for (int v = 0; v < 16; v++) {
        float w = fminf(1.f, k_reach[hops(v, g->grab)] * (v == g->grab ? 1.f : eager)) * smooth(g->pull_k);
        add(v, dx * w, dy * w, .35f * w);
    }
}
// She is soft: tilted, every corner sags downhill, the outer cell more, each a little differently.
static void sag(const tess_games_t *g) {
    // A hand never holds still: below a clear tilt she keeps her shape, so the springs can rest (the fast path).
    if (fabsf(g->tilt[0]) + fabsf(g->tilt[1]) < .1f) return;
    for (int v = 0; v < 16; v++) {
        float give = (v & 8 ? .5f : .3f) * (1.f + .35f * tess_sin(v * 1.7f));
        add(v, g->tilt[0] * give, g->tilt[1] * give, 0);
    }
}
static void squash_side(const tess_games_t *g, const float (*home)[3]) {
    if (g->squash_k <= .001f) return;
    float nx = g->squash[0], ny = g->squash[1];
    for (int v = 0; v < 16; v++) {
        float into = home[v][0] * nx + home[v][1] * ny;  // how far this corner reached towards the wall
        if (into <= 0) continue;
        float flat = into * g->squash_k * .6f;
        add(v, -nx * flat - ny * flat * .5f, -ny * flat + nx * flat * .5f, 0);  // flattened, and squeezed sideways
    }
}
static void wrestle(const tess_games_t *g, const float (*dir)[2]) {
    if (g->self_act != SELF_WRESTLE) return;
    float t = g->self_age, e = t < .9f ? smooth(t / .9f) : 0;  // they strain apart, then let go at once
    for (int side = 0; side < 2; side++) {
        int v = side ? g->self_vertex ^ 15 : g->self_vertex;
        add(v, dir[v][0] * .9f * e, dir[v][1] * .9f * e, .2f * e);
    }
}

// A Tess still growing is folded small; her play flashes open along the tesseract she will become,
// so even a point rings, flowers and reaches with the full shape (inner and outer cell). The same corners,
// on the panel, are what touch looks up.
static void unfold_home(tess_games_t *g, const float points[TESS_N][3], float home[16][3]) {
    float unfolded = 1 - clampf(g->form / 3.f, 0, 1);
    for (int v = 0; v < 16; v++) {
        float cell = v & 8 ? 1.f : .55f;
        for (int axis = 0; axis < 3; axis++)
            home[v][axis] = points[k_slots.vertex_slot[v]][axis] + unfolded * cell * (v & (1 << axis) ? 1.f : -1.f);
    }
}
static bool rings(const tess_games_t *g) {
    bool any = g->self_act == SELF_WRESTLE;
    for (int w = 0; w < TESS_WAVES; w++) any = any || g->wave[w].amp != 0;
    return any;
}
void tess_play_frame(face_t *f, const float points[TESS_N][3], float k, float bx, float by, float pc, float ps) {
    if (!k_slots.ready) map_slots();
    tess_games_t *g = &f->tess_games;
    memset(s_move, 0, sizeof s_move);
    s_moving = false;
    if (!g->ready || f->tess_fallen) { g->shaping = false; return; }
    float home[16][3], dir[16][2];
    unfold_home(g, points, home);
    if (rings(g)) {
        for (int v = 0; v < 16; v++) radial(home[v], dir[v]);
        ring_waves(g, (const float (*)[2])dir);
        wrestle(g, (const float (*)[2])dir);
    }
    open_bloom(g, home);
    pull_vertex(g, home, fmaxf(k, .05f), bx, by, pc, ps);
    squash_side(g, home);
    sag(g);
    for (int v = 0; v < 16 && !s_moving; v++)
        s_moving = fabsf(s_move[v][0]) + fabsf(s_move[v][1]) + fabsf(s_move[v][2]) > 1e-4f;
    g->shaping = s_moving;
}

void tess_play_point(int slot, float *x, float *y, float *z) {
    if (!s_moving || slot < 0 || slot >= TESS_N) return;
    const float *a = s_move[k_slots.a[slot]], *b = s_move[k_slots.b[slot]];
    float t = k_slots.t[slot];
    *x += a[0] + (b[0] - a[0]) * t;
    *y += a[1] + (b[1] - a[1]) * t;
    *z += a[2] + (b[2] - a[2]) * t;
}

int tess_play_nearest_vertex(face_t *f, float x, float y, float *distance) {
    const tess_games_t *g = &f->tess_games;
    int best = 0;
    float near = 1e9f;
    for (int v = 0; v < 16; v++) {
        float d = hypotf(g->vertex_px[v][0] - x, g->vertex_px[v][1] - y);
        if (d < near) { near = d; best = v; }
    }
    if (distance) *distance = near;
    return best;
}

// A new pluck. Two waves from different corners that are both still travelling meet somewhere inside her:
// where they arrive together their rings add up. Finding that is a discovery. `pace` < 1 is quicker and higher.
void tess_play_pluck_as(face_t *f, int vertex, bool quiet, float amp, float pace) {
    tess_games_t *g = &f->tess_games;
    tess_wave_t *slot = NULL, *oldest = &g->wave[0];
    bool echo = false;
    for (int w = 0; w < TESS_WAVES; w++) {
        tess_wave_t *wave = &g->wave[w];
        if (wave->amp != 0 && wave->age < 3 * wave->shell_s && wave->origin != (vertex & 15) && !wave->quiet) echo = true;
        if (wave->amp == 0 && !slot) slot = wave;  // a free slot first, else the oldest wave makes way
        if (wave->age > oldest->age) oldest = wave;
    }
    if (!slot) slot = oldest;
    pace = clampf(pace, .4f, 1.6f);
    *slot = (tess_wave_t){(uint8_t)(vertex & 15), quiet, 0, 0, quiet ? .6f * amp : amp, SHELL_S * pace, RING_W / pace, 1.f / pace};
    if (quiet) return;
    tess_games_earn(f, BIT_PLUCK);
    if (echo) tess_games_earn(f, BIT_ECHO);
}
void tess_play_pluck(face_t *f, int vertex, bool quiet) { tess_play_pluck_as(f, vertex, quiet, 1.f, 1.f); }
// A wave sounds once, as it starts: her whole body resonates (tess_sound_body.c). The pitch is hers, not the
// corner's, so plucks never spell a tune: the corner picks which overtone rings, a quick wave sits a fifth up,
// a recoil a fourth down.
static void ring_wave(face_t *f, const tess_wave_t *wave) {
    float hz = tess_tone_hz(f, wave->pitch > 1.3f ? 1.f : 0) * (wave->amp < 0 ? .75f : 1.f);
    tess_tone(f, TT_RING, hz, fabsf(wave->amp) * .8f, (float)((wave->origin * 7) & 15) / 15.f);
}
void tess_play_waves(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    for (int w = 0; w < TESS_WAVES; w++) {
        tess_wave_t *wave = &g->wave[w];
        if (wave->amp == 0) continue;
        wave->age += dt;
        if (wave->age >= 4 * wave->shell_s + 1.3f) { wave->amp = 0; continue; }
        for (int h = 0; h <= 4; h++) {
            if (wave->age < h * wave->shell_s || wave->rung & (1u << h)) continue;
            wave->rung |= (uint8_t)(1u << h);
            if (h == 4 && wave->amp > 0) g->pulse = fmaxf(g->pulse, .7f * wave->amp);  // every path meets at the far corner
            if (!wave->quiet && h == 0) ring_wave(f, wave);
        }
    }
}
// Spin opens her: the faster she turns, the further the outer cell flowers out.
void tess_play_spin(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    float spin = hypotf(f->tess_mood.drag_speed[0], f->tess_mood.drag_speed[1]);
    g->spin = spin;
    bool allowed = tess_games_eligible(f);
    float target = allowed ? smooth((spin - 3.5f) / 6.5f) * (1.f - .6f * g->annoy) : 0;  // a few lively swipes open her
    g->bloom += (target - g->bloom) * fminf(1, dt * 5.f);
    if (g->bloom > .8f && !(g->progress & (1u << BIT_BLOOM))) tess_tone(f, TT_CHIRP, tess_tone_hz(f, .8f), .5f, .9f);
    if (g->bloom > .8f) tess_games_earn(f, BIT_BLOOM);
    // Air past her edges: a whoosh every half turn, higher and louder the faster she goes.
    g->whoosh_turn += spin * dt;
    if (allowed && spin > 4.f && g->whoosh_turn >= 2 * PI) tess_tone(f, TT_WHOOSH, 160.f + 20.f * spin, fminf(1, spin / 16.f) * .7f, g->bloom);
    if (g->whoosh_turn >= 2 * PI || spin <= 4.f) g->whoosh_turn = 0;
}
