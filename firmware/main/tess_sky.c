#include "tess_sky_play.h"

#include <stdlib.h>
#include <string.h>

// Skies, the rounds. She asks with light from her corner and her motif (tess_sky_summon.c); answered, she unravels: the burst
// throws her dots out, and where they hang the stream takes them. From then on the sky is a puzzle of light
// (tess_sky_stream.c, tess_sky_compose.c). All stars full for 1.6 s resolves it: the sky sings its own melody, star by
// star as a line of dots joins them, closes on its chord, and the next sky grows from it with one thing more. Her dots are the
// burst's own dots at their slots, so leaving, for any reason, lets the burst finish: they spring home from where they are.
//   A touch or a tilt is all it needs to stay; 15 s of neither fades it out. Nothing is ever lost or wrong.
#define SKIES_PER_VISIT 6
#define AURORA_S 40.f
#define CALM_FADE_S 15.f
#define TILT_QUIET .12f
#define POUR_TILT 1.f
#define PUZZLE_TILT .3f

// ---------------------------------------------------------------- what is kept
// elements met (4 bits), skill (8), chord root (3), skies solved (16).
void tess_games_restore_sky(face_t *f, uint32_t packed) {
    tess_games_t *g = &f->tess_games;
    unsigned elements = packed & 15u, root = (packed >> 12) & 7u;
    if (elements > EL_COUNT || root > 4) return;
    g->sky_elements = (uint8_t)elements;
    g->sky_skill = (uint8_t)(packed >> 4);
    g->sky_root = (uint8_t)root;
    g->sky_solved = (uint16_t)(packed >> 15);
}
uint32_t tess_games_sky_progress(const face_t *f, bool *settled) {
    const tess_games_t *g = &f->tess_games;
    *settled = !g->sky || (g->sky->phase != SKY_PLAY && g->sky->phase != SKY_UNRAVEL);
    return (uint32_t)g->sky_elements | (uint32_t)g->sky_skill << 4 | (uint32_t)g->sky_root << 12 | (uint32_t)g->sky_solved << 15;
}

bool tess_games_end_sky(face_t *f) {
    tess_sky_t *k = f->tess_games.sky;
    if (!k || f->tess_games.game != TESS_GAME_SKY) return false;
    if (k->phase == SKY_UNRAVEL) {   // no stream yet: her dots simply spring home
        tess_sky_stop(f);
        tess_games_leave(&f->tess_games);
    } else if (k->phase != SKY_FADE) {
        k->phase = SKY_FADE;
        k->fade = 0;
    }
    return true;
}

// ---------------------------------------------------------------- lifetime
bool tess_sky_start(face_t *f) {
    tess_games_t *g = &f->tess_games;
    if (!tess_games_eligible(f) || !f->tess_points_ready || f->tess_hold[TR_SCATTER] > 0) return false;
    tess_sky_t *k = calloc(1, sizeof *k);
    if (!k) return false;
    tess_games_begin(f, TESS_GAME_SKY);
    g->sky = k;
    g->sky_invite_t = 0;
    k->phase = SKY_UNRAVEL;
    k->grab = k->hint_tool = k->tap_star = -1;
    if (g->sky_elements < 1) g->sky_elements = 1;
    tess_scatter_begin(f);
    tess_cue(f, TC_JOY, .5f, 0);
    return true;
}
void tess_sky_release(tess_games_t *g) {
    free(g->sky);
    g->sky = NULL;
}
// A sky that ends for any reason: every voice lets go. (The caller leaves the round.)
void tess_sky_stop(face_t *f) {
    if (f->tess_games.sky) sky_sound_silence(f);
}
// (every frame, whatever the round) voices left sounding after the sky is gone are let go; an invitation runs out
void tess_sky_tick(face_t *f, float dt) {
    if (!f->tess_games.sky && f->tess_games.sky_lit) sky_sound_silence(f);
    sky_summon_tick(f, dt);
}
bool tess_sky_holds_burst(const face_t *f) { return f->tess_games.game == TESS_GAME_SKY && f->tess_games.sky; }

// ---------------------------------------------------------------- a sky
// The next sky grows a slice a frame (tess_sky_compose.c) while her dots fly out, every other frame while the last
// one sings its melody, then a slice a frame again.
static void compose_begin(face_t *f, tess_sky_t *k) {
    tess_games_t *g = &f->tess_games;
    sky_recipe_t r = {.elements = g->sky_elements ? g->sky_elements : 1, .skill = g->sky_skill, .root = g->sky_root % 5};
    r.newest = (uint8_t)(r.elements - 1);
    r.first = r.elements <= 1;
    r.aurora = !r.first && r.elements >= 6 && k->solved_here >= 2 && frand(f) < .09f;
    r.from[0] = frand(f) < .5f ? 88.f : 392.f;
    r.from[1] = SKY_CY + frange(f, -40.f, 40.f);
    if (g->sky_call[0] > 0) { r.from[0] = clampf(g->sky_call[0], 80.f, 400.f); r.from[1] = clampf(g->sky_call[1], 95.f, 415.f); }   // from the star that called
    k->seed = (uint32_t)(frand(f) * 4294967040.f) | 1u;
    sky_compose_begin(&k->comp, &k->next, k->seed, &r);
    k->compose = 1;
}
static void compose_slice(tess_sky_t *k) {
    if (k->compose != 1) return;
    if (sky_compose_step(&k->comp)) k->compose = 2;
}
// The grown sky takes the stage; on the first, her dots are let go from where they hang.
static void make_sky(face_t *f, tess_sky_t *k, bool first_launch) {
    (void)f;
    k->w = k->next;
    k->compose = 0;
    k->hold = k->calm = k->struggle = k->resolve_t = k->demo_t = 0;
    k->hints = 0;
    k->hint_tool = -1;
    k->played = (sky_played_t){0};
    if (!first_launch) return;
    int32_t from[SKY_PARTICLES][2];
    for (int i = 0; i < SKY_PARTICLES; i++) {
        float x, y;
        tess_scatter_spot_px(i, &x, &y);
        from[i][0] = (int32_t)(x * SKY_Q3); from[i][1] = (int32_t)(y * SKY_Q3);
    }
    sky_stream_launch(&k->w, k->p, SKY_PARTICLES, from);
}
// Her dots are the burst's slots: each takes the place of its particle.
static void publish(const tess_sky_t *k) {
    for (int i = 0; i < SKY_PARTICLES; i++)
        tess_scatter_hang_at(i, (float)k->p[i].x * (1.f / SKY_Q3), (float)k->p[i].y * (1.f / SKY_Q3));
}

static float hint_delay_s(int skill) { return 60.f - 40.f * (float)skill / 255.f; }
// The thing furthest from where the solution has it trembles towards it: a tool, else a relay turned the wrong way,
// else the aimed source.
static void show_hint(tess_sky_t *k) {
    const sky_world_t *w = &k->w;
    float worst = 20.f;
    k->hint_tool = -1;
    for (int i = 0; i < w->count_tools; i++) {
        const sky_tool_t *t = &w->tool[i];
        float d = t->movable ? hypotf(t->x - t->sx, t->y - t->sy) + 40.f * hypotf(t->dx - t->sdx, t->dy - t->sdy) / 60.f : 0;
        if (d > worst) { worst = d; k->hint_tool = (int8_t)i; }
    }
    for (int i = 0; k->hint_tool < 0 && i < w->count_stars; i++)
        if (w->star[i].relay && w->star[i].dir != w->star[i].sdir) k->hint_tool = (int8_t)SKY_HINT_RELAY(i);
    if (k->hint_tool < 0 && w->source.aim && w->source.dx * w->source.sdx + w->source.dy * w->source.sdy < .985f) k->hint_tool = SKY_HINT_SOURCE;
    if (k->hint_tool >= 0) k->hints++;
}
// The tool just met slides a little way to where it is wanted, then waits: that shows what it is for.
static void demo(tess_sky_t *k, float dt) {
    if (k->w.kind != SKY_PUZZLE || k->hints || k->solved_here > 20 || k->calm < 3.5f || k->demo_t > 1.6f) return;
    k->demo_t += dt;
    float u = smooth01(k->demo_t / 1.4f) * .45f;
    for (int i = 0; i < k->w.count_tools; i++) {
        sky_tool_t *t = &k->w.tool[i];
        if (!t->movable || i != k->w.count_tools - 1 || k->grab == i) continue;
        if (k->demo_t <= dt * 1.5f) { t->shown = 0; k->grab_dx = t->x; k->grab_dy = t->y; }   // (where it started)
        t->x = k->grab_dx + (t->sx - k->grab_dx) * u;
        t->y = k->grab_dy + (t->sy - k->grab_dy) * u;
    }
}

static void begin_resolve(face_t *f, tess_sky_t *k) {
    tess_games_t *g = &f->tess_games;
    bool aurora = k->w.kind == SKY_AURORA;
    k->phase = SKY_RESOLVE;
    k->resolve_t = 0;
    sky_melody_compose(&k->w, &k->played, k->seed + 977u * k->solved_here, &k->melody);
    k->melody_next = k->tour_lit = 0;
    k->resolve_len = SKY_MELODY_AT_S + sky_melody_length_ms(&k->melody) / 1000.f + SKY_CHORD_RING_S;
    sky_sound_resolve(f, k);
    g->pulse = 1.f;
    tess_react(f, TR_JOY, 1.f);
    if (aurora) return;
    bool quick = k->struggle < 45.f && !k->hints, slow = k->struggle > 100.f || k->hints;
    int skill = g->sky_skill + (quick ? 28 : slow ? -34 : 6);
    g->sky_skill = (uint8_t)(skill < 0 ? 0 : skill > 255 ? 255 : skill);
    if (g->sky_elements < EL_COUNT) g->sky_elements++;
    g->sky_solved++;
    k->solved_here++;
    g->sky_root = (uint8_t)((g->sky_root + 2 + (int)(frand(f) * 2.f)) % 5);
}

static bool hint_due(const tess_games_t *g, const tess_sky_t *k) { return k->struggle > hint_delay_s(g->sky_skill) && k->hints < 3; }
static bool gravity_force(const face_t *f, const tess_sky_t *k, sky_force_t *force) {
    const tess_games_t *g = &f->tess_games;
    bool pouring = k->w.kind == SKY_POUR;
    float tilt = pouring ? POUR_TILT : PUZZLE_TILT;
    // Pouring follows gravity on the glass, not the body's slowly adapting rest pose.
    float gx = pouring ? f->grav_x : g->tilt[0], gy = pouring ? f->grav_y : g->tilt[1];
    force->gx = gx * tilt; force->gy = gy * tilt;
    return fabsf(gx) + fabsf(gy) > TILT_QUIET;
}
static void play(face_t *f, tess_sky_t *k, float dt) {
    tess_games_t *g = &f->tess_games;
    sky_force_t force = {0};
    bool touched = sky_touch(f, k, &force, dt);
    bool tilted = gravity_force(f, k, &force);
    k->calm = touched || tilted ? 0.f : k->calm + dt;
    k->struggle += dt;
    sky_hits_t hits;
    sky_stream_step(&k->w, k->p, SKY_PARTICLES, dt, &force, &hits);
    sky_sound_frame(f, k, &hits, dt);
    demo(k, dt);
    for (int i = 0; i < k->w.count_tools; i++) k->w.tool[i].shown = fmaxf(0, k->w.tool[i].shown - dt * 3.f);
    if (k->hint_tool < 0 && k->w.kind != SKY_AURORA && hint_due(g, k)) show_hint(k);
    k->hold = sky_solved(&k->w) ? k->hold + dt : fmaxf(0, k->hold - 2.f * dt);
    bool aurora = k->w.kind == SKY_AURORA;
    if (aurora ? k->w.t > AURORA_S : k->hold >= SKY_HOLD_S) begin_resolve(f, k);
    else if (k->calm > CALM_FADE_S) k->phase = SKY_FADE;
}

static void resolve(face_t *f, tess_sky_t *k, float dt) {
    sky_force_t none = {0};
    sky_hits_t hits;
    k->resolve_t += dt;
    for (int i = 0; i < k->w.count_stars; i++) k->w.star[i].fill = k->w.kind == SKY_AURORA ? k->w.star[i].fill : 1.f;
    sky_stream_step(&k->w, k->p, SKY_PARTICLES, dt, &none, &hits);
    sky_sound_frame(f, k, &hits, dt);
    sky_sound_melody(f, k, (k->resolve_t - SKY_MELODY_AT_S) * 1000.f);
    bool more = k->solved_here < SKIES_PER_VISIT, singing = k->resolve_t < k->resolve_len;
    if (more && !k->compose) compose_begin(f, k);
    if (!singing || (k->slice_turn ^= 1)) compose_slice(k);   // (every other frame while the melody needs the time)
    if (singing) return;
    if (!more) { k->phase = SKY_FADE; return; }
    if (k->compose != 2) return;   // (still growing: the stream flows on)
    make_sky(f, k, false);
    k->phase = SKY_PLAY;
}

static void fade(face_t *f, tess_sky_t *k, float dt) {
    sky_force_t none = {0};
    sky_hits_t hits;
    k->fade += dt;
    sky_stream_step(&k->w, k->p, SKY_PARTICLES, dt, &none, &hits);
    sky_sound_frame(f, k, &hits, dt);
    if (k->fade < SKY_FADE_S) return;
    tess_sky_stop(f);
    tess_games_leave(&f->tess_games);
}

static void unravel(face_t *f, tess_sky_t *k) {
    if (!k->compose) compose_begin(f, k);
    compose_slice(k);
    if (TESS_BURST_S - f->tess_hold[TR_SCATTER] < TESS_HANG_S - .01f || k->compose != 2) return;   // her dots are still flying out
    make_sky(f, k, true);
    k->phase = SKY_PLAY;
}

void tess_sky_update(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    tess_sky_t *k = g->sky;
    if (!k) { tess_games_leave(g); return; }
    g->age += dt;
    g->clock += dt;
    switch (k->phase) {
    case SKY_UNRAVEL: unravel(f, k); break;
    case SKY_PLAY: play(f, k, dt); break;
    case SKY_RESOLVE: resolve(f, k, dt); break;
    default: fade(f, k, dt);
    }
    if (g->sky && k->phase != SKY_UNRAVEL) publish(k);
}
