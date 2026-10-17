// Simulator scenario for Skies: the invitation, the tap on its gold light, the unravelling, a first sky solved with a
// finger, the resolution and the next sky solved by putting its tools where its composer says (a player's moves are
// not scripted). Frames go to stdout, the sky's timeline to stderr.
//   sim sky [seconds]
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "../main/tess_sky_play.h"

static int tones[TT_COUNT];

static void feed(face_t *f, bool down, float x, float y) { f->rub_in = (rub_input_t){.down = down, .x = x, .y = y}; }
static void step(face_t *f, float t, const char *what) {
    face_update(f, 1.f / R_FPS);
    tess_tone_t tone;
    while (face_take_tone(f, &tone)) tones[tone.kind]++;
    tess_cue_t cue; float s, p;
    while (face_take_cue(f, &cue, &s, &p)) {}
    sim_render(f);
    sim_emit_frame();
    (void)t; (void)what;
}
static void report(face_t *f, float t) {
    const tess_sky_t *k = f->tess_games.sky;
    if (!k) { fprintf(stderr, "%5.1fs no sky\n", t); return; }
    fprintf(stderr, "%5.1fs phase %d kind %d stars", t, k->phase, k->w.kind);
    for (int i = 0; i < k->w.count_stars; i++) fprintf(stderr, " %.2f", k->w.star[i].fill);
    fprintf(stderr, " tools %d relays/aim %d/%d melody %d/%d solved %d elements %d skill %d\n", k->w.count_tools,
            (int)(k->w.star[0].relay + k->w.star[1].relay + k->w.star[2].relay), k->w.source.aim, k->melody_next, k->melody.n, f->tess_games.sky_solved, f->tess_games.sky_elements, f->tess_games.sky_skill);
}

// The relays and an aimed source, a step at a time, as a player would turn them (after a moment's look).
static void turn_things(sky_world_t *w) {
    for (int s = 0; s < w->count_stars; s++)
        if (w->star[s].relay && w->star[s].dir != w->star[s].sdir && w->t > 2.f + s && fmodf(w->t, .6f) < 1.f / R_FPS) w->star[s].dir = (uint8_t)((w->star[s].dir + 1) % SKY_DIRS);
    if (!w->source.aim || w->t < 1.5f) return;
    w->source.dx += (w->source.sdx - w->source.dx) * .06f; w->source.dy += (w->source.sdy - w->source.dy) * .06f;
    float len = hypotf(w->source.dx, w->source.dy);
    w->source.dx /= len; w->source.dy /= len;
}
static bool needs_finger(const sky_world_t *w) {
    if (w->count_tools || w->source.aim) return false;
    for (int s = 0; s < w->count_stars; s++) if (w->star[s].relay) return false;
    return true;
}
// What a player would do in this sky: tilt a pour, put a finger on the emptiest star, or move each tool to its place.
static void play_it(face_t *f, tess_games_t *g) {
    sky_world_t *w = &g->sky->w;
    turn_things(w);
    if (w->kind == SKY_POUR) {
        f->grav_x = g->tilt_rest[0] + w->gravity[0] + copysignf(.06f, w->gravity[0]);
        f->grav_y = g->tilt_rest[1] + w->gravity[1] + copysignf(.06f, w->gravity[1]);
    } else if (needs_finger(w)) {
        int star = 0;
        for (int s = 1; s < w->count_stars; s++) if (w->star[s].fill < w->star[star].fill) star = s;
        feed(f, true, w->star[star].x, w->star[star].y);
    } else {
        tess_sky_t *m = g->sky;
        for (int s = 0; s < m->w.count_tools; s++) {
            sky_tool_t *tl = &m->w.tool[s];
            if (!tl->movable) continue;
            tl->x += (tl->sx - tl->x) * .08f; tl->y += (tl->sy - tl->y) * .08f;
            tl->dx += (tl->sdx - tl->dx) * .08f; tl->dy += (tl->sdy - tl->dy) * .08f;
        }
    }
}

static bool tap_corner(face_t *f, float t) {
    const tess_games_t *g = &f->tess_games;
    const float *v = g->sky_call;
    face_event(f, FEV_TAP, v[0], v[1]);
    fprintf(stderr, "%5.1fs tap on the summoning star (%.0f,%.0f): sky %s\n", t, v[0], v[1], g->sky ? "starts" : "NOT started");
    return true;
}
static void begin(face_t *f) {
    face_init(f);
    face_set_character(f, CHARACTER_TESS);
    face_set_mode(f, MODE_IDLE);
    tess_games_restore(f, getenv("SKY_PROGRESS") ? (uint8_t)atoi(getenv("SKY_PROGRESS")) : 0);
    if (getenv("SKY_ELEMENTS")) f->tess_games.sky_elements = (uint8_t)atoi(getenv("SKY_ELEMENTS"));   // start deeper in the ladder
    if (getenv("SKY_SKILL")) f->tess_games.sky_skill = (uint8_t)atoi(getenv("SKY_SKILL"));
    for (int i = 0; i < 8 * R_FPS; i++) face_update(f, 1.f / R_FPS);
    f->tess_games.self_wait = 999;
    while (!tess_sky_invite(f)) {}
}
void sim_sky(float seconds) {
    face_t f;
    begin(&f);
    int frames = (int)(seconds * R_FPS);
    bool tapped = false;
    for (int i = 0; i < frames; i++) {
        float t = (float)i / R_FPS;
        tess_games_t *g = &f.tess_games;
        const tess_sky_t *k = g->sky;
        bool playing = k && k->phase == SKY_PLAY;
        feed(&f, false, 0, 0);
        if (!getenv("SKY_INVITE_ONLY") && !tapped && t > 1.5f) tapped = tap_corner(&f, t);
        if (!(playing && k->w.kind == SKY_POUR)) { f.grav_x = g->tilt_rest[0]; f.grav_y = g->tilt_rest[1]; }
        if (playing) play_it(&f, g);
        step(&f, t, "sky");
        if (i % R_FPS == 0) report(&f, t);
    }
    fprintf(stderr, "tones:");
    for (int k = 0; k < TT_COUNT; k++) fprintf(stderr, " %d", tones[k]);
    fprintf(stderr, "\n");
}
