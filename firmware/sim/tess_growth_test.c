// Tess's native growth keeps one 4D point lattice: point, square, cube, tesseract.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../main/tess_internal.h"

static face_t fresh(void) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    f.tess_assemble = 1;
    return f;
}

static void copy_band(int x0, int y0, int x1, int y1, uint16_t *pixels, void *context) {
    uint16_t *frame = context;
    for (int y = y0; y < y1; y++)
        memcpy(frame + y * R_W + x0, pixels + (y - y0) * R_W, (size_t)(x1 - x0) * sizeof *pixels);
}

static void rasterize(const scene_t *scene, uint16_t frame[R_W * R_H]) {
    static uint16_t bands[2][R_W * R_BAND];
    uint16_t *buffers[] = {bands[0], bands[1]};
    render_state_t state = {0};
    render_frame(&state, scene, buffers, 2, false, copy_band, frame);
}

static int unique_vertices(const float v[16][4]) {
    float unique[16][4];
    int n = 0;
    for (int i = 0; i < 16; i++) {
        bool seen = false;
        for (int j = 0; j < n && !seen; j++) {
            float d = 0;
            for (int a = 0; a < 4; a++) d = fmaxf(d, fabsf(v[i][a] - unique[j][a]));
            seen = d < 1e-5f;
        }
        if (!seen) memcpy(unique[n++], v[i], sizeof unique[0]);
    }
    return n;
}

static void vertices_are_finite(const float v[16][4]) {
    for (int i = 0; i < 16; i++)
        for (int a = 0; a < 4; a++) assert(isfinite(v[i][a]) && fabsf(v[i][a]) <= 2.001f);
}

static void adult_projection_is_unchanged(void) {
    face_t f = fresh();
    float old[16][4], got[16][4];
    f.tess_xw = .81f; f.tess_zw = -.42f; f.tess_mood.yw = .33f;
    f.tess_mood.w_turn[0] = .15f; f.tess_mood.w_turn[1] = -.2f; f.tess_mood.w_turn[2] = .08f;
    tess_vertices4d(&f, old);
    f.tess_games.form = 0; f.tess_games.fold = 1; f.tess_games.offset[0] = .3f; f.tess_games.pulse = 1;
    tess_growth_vertices(&f, got);  // dormant fixtures never inherit app progress
    assert(!memcmp(old, got, sizeof old));
    f.tess_games.ready = true; f.tess_games.form = 3; f.tess_games.fold = 0;
    f.tess_games.offset[0] = f.tess_games.offset[1] = 0;
    tess_growth_vertices(&f, got);
    assert(!memcmp(old, got, sizeof old));
}

static void dimensions_open_in_order(void) {
    face_t f = fresh();
    float v[16][4];
    f.tess_games.ready = true;
    // Even a queued playful 4D pose waits for the fourth dimension, keeping the first two shapes recognizable.
    f.tess_xw = .8f; f.tess_zw = -.5f; f.tess_mood.yw = .7f;
    f.tess_mood.w_turn[0] = .4f; f.tess_mood.w_turn[1] = -.3f; f.tess_mood.w_turn[2] = .2f;
    f.style.xw = .25f; f.tess_mode[TM_THINK] = .5f;

    f.tess_games.form = 0;
    tess_growth_vertices(&f, v); vertices_are_finite(v);
    assert(unique_vertices(v) == 1);
    for (int i = 0; i < 16; i++) for (int a = 0; a < 4; a++) assert(v[i][a] == 0.f);

    f.tess_games.form = 1;
    tess_growth_vertices(&f, v); vertices_are_finite(v);
    assert(unique_vertices(v) == 4);
    for (int i = 0; i < 16; i++) {
        assert(fabsf(fabsf(v[i][0]) - 1.f) < 1e-5f && fabsf(fabsf(v[i][1]) - 1.f) < 1e-5f);
        assert(fabsf(v[i][2]) < 1e-5f && fabsf(v[i][3]) < 1e-5f);
    }

    f.tess_games.form = 2;
    tess_growth_vertices(&f, v); vertices_are_finite(v);
    assert(unique_vertices(v) == 8);
    for (int i = 0; i < 16; i++) {
        for (int a = 0; a < 3; a++) assert(fabsf(fabsf(v[i][a]) - 1.f) < 1e-5f);
        assert(fabsf(v[i][3]) < 1e-5f);
    }

    // Partial stages stay in bounds and interpolate the newly opened axis continuously.
    f.tess_xw = f.tess_zw = f.tess_mood.yw = 0;
    memset(f.tess_mood.w_turn, 0, sizeof f.tess_mood.w_turn); f.style.xw = 0; f.tess_mode[TM_THINK] = 0;
    for (int step = 1; step < 30; step++) {
        f.tess_games.form = step * .1f;
        tess_growth_vertices(&f, v); vertices_are_finite(v);
        if (f.tess_games.form < 1) assert(fabsf(fabsf(v[0][0]) - f.tess_games.form) < 1e-5f);
        else if (f.tess_games.form < 2) assert(fabsf(fabsf(v[0][2]) - (f.tess_games.form - 1)) < 1e-5f);
        else assert(fabsf(fabsf(v[0][3]) - (f.tess_games.form - 2)) < 1e-5f);
    }
}

static void fold_is_bounded_and_returns_home(void) {
    face_t f = fresh();
    float old[16][4], got[16][4];
    f.tess_games.ready = true; f.tess_games.form = 3;
    tess_vertices4d(&f, old);
    for (int step = 0; step <= 10; step++) {
        float fold = step * .1f;
        f.tess_games.fold = fold;
        tess_growth_vertices(&f, got); vertices_are_finite(got);
        for (int i = 0; i < 16; i++)
            for (int a = 0; a < 4; a++) assert(fabsf(got[i][a] - old[i][a] * (1 - fold)) < 2e-5f);
    }
    f.tess_games.form = NAN; f.tess_games.fold = 0;
    tess_growth_vertices(&f, got);
    assert(!memcmp(old, got, sizeof old));
    f.tess_games.form = 3; f.tess_games.fold = NAN;
    tess_growth_vertices(&f, got);
    assert(!memcmp(old, got, sizeof old));
    f.tess_games.form = 2.5f; f.tess_games.fold = 0.f; f.tess_xw = NAN;
    tess_growth_vertices(&f, got);
    vertices_are_finite(got);
}

static void center_offset_is_opt_in(void) {
    face_t base = fresh(), moved = fresh();
    float a[TESS_N][3], b[TESS_N][3];
    moved.tess_games.offset[0] = .2f; moved.tess_games.offset[1] = -.1f;
    tess_point_targets(&moved, b, -1);
    tess_point_targets(&base, a, -1);
    assert(!memcmp(a, b, sizeof a));
    moved.tess_games.ready = true; moved.tess_games.form = 3;
    tess_point_targets(&moved, b, -1);
    for (int i = 0; i < TESS_N; i++) {
        assert(fabsf((b[i][0] - a[i][0]) - .2f) < 1e-5f);
        assert(fabsf((b[i][1] - a[i][1]) + .1f) < 1e-5f);
        assert(fabsf(b[i][2] - a[i][2]) < 1e-5f);
    }
}

static void adult_renderer_is_exact_when_progress_is_ready(void) {
    face_t dormant = fresh(), adult = fresh();
    adult.tess_games.ready = true;
    adult.tess_games.form = 3.f;
    scene_t a = {0}, b = {0};
    scene_begin(&a, 0); scene_begin(&b, 0);
    tess_draw(&dormant, &a);
    tess_draw(&adult, &b);
    assert(a.dots_n == TESS_N && b.dots_n == TESS_N);
    assert(!memcmp(a.dots, b.dots, sizeof a.dots[0] * TESS_N));
}

static void a_collapsed_game_point_is_drawn_once(void) {
    face_t f = fresh();
    f.tess_games.ready = true; f.tess_games.form = 0; f.tess_games.pulse = .5f;
    f.tess_points_ready = true;
    memset(f.tess_position, 0, sizeof f.tess_position);
    memset(f.tess_wdepth, 0, sizeof f.tess_wdepth);
    tess_projected_t projected[TESS_N];
    tess_project_dots(&f, projected);
    assert(f.tess_games.hit_valid && fabsf(f.tess_games.hit[0] - 240.f) < .125f &&
           fabsf(f.tess_games.hit[1] - 255.f) < .125f);
    f.tess_games.offset[0] = .4f; f.tess_games.offset[1] = -.2f;
    tess_project_dots(&f, projected);
    int64_t x = 0, y = 0;
    for (int i = 0; i < TESS_N; i++) { x += projected[i].x; y += projected[i].y; }
    assert(f.tess_games.hit[0] == (float)(x / TESS_N) * (1.f / 8.f));
    assert(f.tess_games.hit[1] == (float)(y / TESS_N) * (1.f / 8.f));
    f.tess_games.offset[0] = f.tess_games.offset[1] = 0;
    tess_project_dots(&f, projected);
    scene_t scene = {0};
    scene_begin(&scene, 0);
    tess_draw(&f, &scene);
    assert(scene.dots_n == 1);
    int swell_q8 = 256 + (int)lrintf(.5f * .8f * 256.f);
    int expected = (projected[0].radius * swell_q8 + 128) >> 8;
    assert(scene.dots[0].radius == expected && expected > projected[0].radius);

    // Identical pixels in a dormant renderer fixture keep its established behavior.
    f.tess_games.ready = false;
    scene_begin(&scene, 0);
    tess_draw(&f, &scene);
    assert(scene.dots_n == TESS_N);
    assert(!f.tess_games.hit_valid);

    // Offline points keep their individual shimmer and are never coalesced.
    f.tess_games.ready = true;
    f.tess_fallen = true;
    scene_begin(&scene, 0);
    tess_draw(&f, &scene);
    assert(scene.dots_n == TESS_N && !f.tess_games.hit_valid);
}

static void open_forms_draw_multiple_points_without_adult_overdraw(void) {
    face_t f = fresh();
    scene_t scene = {0};
    f.tess_games.ready = true;
    for (int form = 1; form <= 3; form++) {
        f.tess_games.form = (float)form;
        scene_begin(&scene, 0);
        tess_draw(&f, &scene);
        if (form < 3) assert(scene.dots_n > 1 && scene.dots_n < TESS_N);
        else assert(scene.dots_n == TESS_N);
    }
}

static void growth_dedup_preserves_each_form_footprint(void) {
    static uint16_t merged[R_W * R_H], repeated[R_W * R_H];
    for (int form = 0; form < 3; form++) {
        face_t f = fresh();
        f.tess_games.ready = true;
        f.tess_games.form = (float)form;
        f.tess_points_ready = true;
        tess_point_targets(&f, f.tess_target, -1);
        memcpy(f.tess_position, f.tess_target, sizeof f.tess_target);

        face_t reference = f;
        reference.tess_games.ready = false;  // same cached positions, original 112-dot painter
        scene_t a = {0}, b = {0};
        scene_begin(&a, 0); scene_begin(&b, 0);
        tess_draw(&f, &a); tess_draw(&reference, &b);
        assert(a.dots_n < TESS_N && b.dots_n == TESS_N);
        if (form == 0) assert(a.dots_n == 1);
        rasterize(&a, merged); rasterize(&b, repeated);
        int footprint_differences = 0, pixel_differences = 0;
        for (int i = 0; i < R_W * R_H; i++) {
            if ((merged[i] != 0) != (repeated[i] != 0)) footprint_differences++;
            if (merged[i] != repeated[i]) pixel_differences++;
        }
        // Duplicate overdraw changes halo brightness, but not the occupied panel footprint.
        assert(footprint_differences == 0);
        assert(pixel_differences > 0);
        printf("growth form %d: dots %d/%d; pixel deltas %d; footprint deltas %d\n", form,
               a.dots_n, b.dots_n, pixel_differences, footprint_differences);
    }
}

int main(void) {
    adult_projection_is_unchanged();
    dimensions_open_in_order();
    fold_is_bounded_and_returns_home();
    center_offset_is_opt_in();
    adult_renderer_is_exact_when_progress_is_ready();
    a_collapsed_game_point_is_drawn_once();
    open_forms_draw_multiple_points_without_adult_overdraw();
    growth_dedup_preserves_each_form_footprint();
    puts("tess growth ok");
    return 0;
}
