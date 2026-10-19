// Real Skies, including invitation and resolution; scripted input follows the composer's verified route.
// Frames go to stdout, the timeline to stderr. sim sky [seconds]
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "../main/tess_sky_play.h"

static int tones[TT_COUNT];

static void feed(face_t *f, bool down, float x, float y) {
    f->rub_in = (rub_input_t){.down = down, .x = x, .y = y};
    f->touch_in = (touch_frame_t){.point={{.down=down,.x=x,.y=y}}};
}
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
    fprintf(stderr, " holes %d melody %d/%d solved %d lessons %d skill %d\n", k->w.count_holes,
            k->melody_next, k->melody.n, f->tess_games.sky_solved, f->tess_games.sky_elements, f->tess_games.sky_skill);
}

static void play_it(face_t *f, tess_games_t *g) {
    const sky_world_t *w = &g->sky->w;
    sky_force_t force;
    sky_solution(w, &force);
    float scale = w->kind == SKY_POUR ? 1.f : SKY_TILT_SCALE;
    f->grav_x = force.gx / scale; f->grav_y = force.gy / scale;
    if (force.count) feed(f, true, force.touch[0].x, force.touch[0].y);
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
