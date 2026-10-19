// A composer's fresh-source proof must remain true through the real runtime transition.
// Initial dots still come from Tess; later worlds reset only after every old light is invisible.
#include "../main/tess_sky_play.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static face_t face;
static scene_t scene;

static void frame(void) {
    face_update(&face, 1.f / 30);
    tess_tone_t tone; while (face_take_tone(&face, &tone)) {}
    tess_cue_t cue; float a, b; while (face_take_cue(&face, &cue, &a, &b)) {}
}

static tess_sky_t *begin(unsigned seed, bool check_origin) {
    tess_games_set_available(&face, false);
    face_init(&face); face_set_character(&face, CHARACTER_TESS); face_set_mode(&face, MODE_IDLE);
    tess_games_restore(&face, TESS_PROGRESS_MASK);
    face.rng = seed * 2654435761u;
    face.tess_games.sky_elements = 1; face.tess_games.sky_skill = 200;
    for (int i = 0; i < 240; i++) frame();
    face.tess_games.self_wait = 999;
    face.tess_games.gather_clock = 4;
    for (int i = 0; i < TESS_N; i++) face.tess_games.gathered_at[i] = 1;
    assert(tess_sky_start(&face));
    assert(face.tess_games.gather_clock == 0);
    for (int i = 0; i < TESS_N; i++) assert(face.tess_games.gathered_at[i] == 0);
    tess_sky_t *k = face.tess_games.sky;
    int32_t from[SKY_PARTICLES][2] = {{0}};
    for (int n = 0; n < 1000 && k->phase == SKY_UNRAVEL; n++) {
        for (int i = 0; check_origin && i < SKY_PARTICLES; i++) {
            float x, y; tess_scatter_spot_px(i, &x, &y);
            from[i][0] = (int32_t)(x * SKY_Q3); from[i][1] = (int32_t)(y * SKY_Q3);
        }
        frame();
    }
    assert(k->phase == SKY_PLAY);
    for (int i = 0; check_origin && i < SKY_PARTICLES; i++)
        assert(k->p[i].x == from[i][0] && k->p[i].y == from[i][1] && !k->p[i].wait);
    return k;
}

static void release_input(void) {
    face.grav_x = face.grav_y = 0;
    face.touch_in = (touch_frame_t){0}; face.rub_in = (rub_input_t){0};
}

static void solve_first(tess_sky_t *k) {
    for (int i = 0; i < 900 && k->phase == SKY_PLAY; i++) {
        sky_force_t force; sky_solution(&k->w, &force);
        float scale = k->w.kind == SKY_POUR ? 1 : SKY_TILT_SCALE;
        face.grav_x = force.gx / scale; face.grav_y = force.gy / scale;
        face.touch_in.point[0] = (touch_point_t){.x = force.touch[0].x, .y = force.touch[0].y, .down = true};
        frame();
    }
    assert(k->phase == SKY_RESOLVE);
    release_input();
}

static void assert_not_passive(tess_sky_t *k) {
    release_input();
    unsigned solved = face.tess_games.sky_solved;
    for (int i = 0; i < 451 && k->phase == SKY_PLAY; i++) frame();
    assert(k->phase != SKY_RESOLVE && face.tess_games.sky_solved == solved);
}

static void the_next_sky_needs_its_own_input(void) {
    // Before this fix all three seeds passed the second sky by themselves in under three seconds.
    const unsigned seeds[] = {1, 5, 6};
    for (unsigned i = 0; i < sizeof seeds / sizeof *seeds; i++) {
        tess_sky_t *k = begin(seeds[i], false);
        solve_first(k);
        for (int n = 0; n < 1000 && k->phase == SKY_RESOLVE; n++) frame();
        assert(k->phase == SKY_PLAY && face.tess_games.sky_solved == 1);
        assert_not_passive(k);
    }
}

static void draw_game(void) {
    scene_begin(&scene, 0);
    tess_draw(&face, &scene);
}

static bool observe_fade(tess_sky_t *k, float *first, unsigned *previous) {
    if (k->fade <= 0) return false;
    assert(k->next_ready && k->resolve_t >= k->resolve_len);
    if (*first < 0) *first = face.t;
    unsigned alpha = tess_sky_alpha(&face);
    assert(alpha <= *previous); *previous = alpha;
    if (k->fade < SKY_FADE_S) return false;
    assert(alpha == 0);
    draw_game(); assert(scene.dots_n == 0 && scene.n == 0);
    return true;
}

static void waiting_particles_are_hidden(tess_sky_t *k) {
    // Waiting source particles cannot form a bright pile before being emitted.
    for (int i = 0; i < SKY_PARTICLES; i++) k->p[i].wait = 10;
    draw_game(); assert(scene.dots_n == 0);
    k->p[face.tess_scatter_of[0]].wait = 0;
    draw_game(); assert(scene.dots_n == 1);
    k->phase = SKY_UNRAVEL;
    draw_game(); assert(scene.dots_n == SKY_PARTICLES && tess_sky_alpha(&face) == 255);
    k->phase = SKY_FADE; k->fade = SKY_FADE_S;
    draw_game(); assert(scene.dots_n == SKY_PARTICLES && tess_sky_alpha(&face) == 255);
}

static void changing_the_source_happens_in_the_dark(void) {
    tess_sky_t *k = begin(1, true);
    solve_first(k);
    bool dark = false;
    float first_fade_at = -1;
    unsigned previous = 255;
    sky_world_t expected = {0}; sky_particle_t fresh[SKY_PARTICLES];
    for (int n = 0; n < 1000 && k->phase == SKY_RESOLVE; n++) {
        if (observe_fade(k, &first_fade_at, &previous)) {
            dark = true;
            expected = k->next;
            sky_stream_launch(&expected, fresh, SKY_PARTICLES, NULL);
        }
        frame();
    }
    assert(dark && k->phase == SKY_PLAY && face.t - first_fade_at > .5f && face.t - first_fade_at < .8f);
    assert(k->w.t == 0 && k->w.rng == expected.rng && !memcmp(k->p, fresh, sizeof fresh));
    waiting_particles_are_hidden(k);
}

static void the_initial_burst_still_needs_input(unsigned seeds) {
    for (unsigned seed = 1; seed <= seeds; seed++) assert_not_passive(begin(seed, seed == 1));
    printf("Initial burst: %u real runtime seeds, no passive solves\n", seeds);
}

int main(int argc, char **argv) {
    the_next_sky_needs_its_own_input();
    changing_the_source_happens_in_the_dark();
    the_initial_burst_still_needs_input(argc > 1 ? (unsigned)atoi(argv[1]) : 16);
    tess_games_set_available(&face, false);
    puts("Skies transition: input required, original burst retained, source reset hidden, waiting particles invisible");
}
