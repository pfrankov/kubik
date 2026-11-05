// Real character/world paths use the visible rectangle without changing object scale.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../main/tess_sky_play.h"
#include "../main/tess_fall.c"
#include "../main/face_internal.h"
#include "../main/sprite.h"

static face_t face;
static void fresh(void) {
    face_init(&face);
    face_set_character(&face, CHARACTER_TESS);
    tess_games_restore(&face, TESS_PROGRESS_MASK);
    face_set_mode(&face, MODE_IDLE);
    face.tess_next_fidget = 1e9f;
    for (int i = 0; i < 240; i++) face_update(&face, 1.f / 30);
}
static void projection_keeps_equal_axes(void) {
    fresh();
    face.tess_fallen = true;
    memset(face.tess_position, 0, sizeof face.tess_position);
    face.tess_position[1][0] = face.tess_position[1][1] = 1;
    tess_projected_t dots[TESS_N]; tess_project_dots(&face, dots);
    assert(dots[0].x == UI_CX * 8 && dots[0].y == TESS_CY_Q3);
    assert(dots[1].x - dots[0].x == dots[1].y - dots[0].y);
    assert(dots[1].radius == dots[0].radius);
    float x, y; tess_games_centre(&face, &x, &y);
    assert(x == UI_CX && y == TESS_CY);
}
static void scatter_uses_the_full_height(void) {
    fresh(); tess_scatter_begin(&face);
    float low = UI_H, high = 0;
    for (int i = 0; i < TESS_N; i++) {
        float x, y; tess_scatter_spot_px(i, &x, &y);
        assert(x >= 16 && x <= UI_W - 16 && y >= 16 && y <= UI_H - 16);
        low = fminf(low, y); high = fmaxf(high, y);
    }
    assert(low < 40 && high > UI_H - 40);
}
static void falling_points_reach_both_edges(void) {
    fresh(); face.grav_y = 1;
    face_set_mode(&face, MODE_OFFLINE);
    for (int side = 0; side < 2; side++) {
        for (int tick = 0; tick < 120; tick++) face_update(&face, 1.f / 30);
        float mean = 0;
        for (int i = 0; i < TESS_N; i++) {
            float z = face.tess_position[i][2];
            float y = TESS_CY + face.tess_position[i][1] * 360.f / (4.7f - z);
            assert(y >= 0 && y < UI_H);
            mean += y / TESS_N;
        }
        assert(side ? mean < 70 : mean > UI_H - 70);
        face.grav_y = -1;
    }
    fall_points_t state = {0}; load_fall(&face, &state);
    assert(fall_grid_row(state.wall[0][1]) > 0);
    assert(fall_grid_row(state.wall[0][2]) < FALL_ROWS - 1);
}
static void body_rim_allows_portrait_travel(void) {
    fresh();
    face.tess_games.game = TESS_GAME_FLY;
    face.tess_games.fly[1] = 10;
    tess_body_update(&face, 1.f / 60);
    float expected = 1.45f + (UI_H - UI_W) / (2.f * TESS_PX_PER_UNIT);
    assert(fabsf(face.tess_games.fly[1] - expected) < .001f);
}
static void skies_bottom_touch_and_catalog(void) {
    fresh(); tess_sky_t sky = {0}; sky_force_t force = {0};
    face.touch_in.point[0] = (touch_point_t){.x=240,.y=UI_H-1,.id=1,.down=true};
    assert(sky_touch(&face, &sky, &force, .1f));
    assert(force.touch[0].y == UI_H - 1 && force.touch[0].shown == 1);
    face.touch_in.point[0].y = UI_H;
    assert(!sky_touch(&face, &sky, &force, .1f));
    for (unsigned chart = 1; chart <= SKY_CHARTS; chart++) {
        sky_world_t w; sky_recipe_t recipe = {.elements=chart};
        sky_compose(&w, 2, &recipe);
        for (int i = 0; i < w.count_stars; i++) {
            assert(w.star[i].y >= SKY_MARGIN && w.star[i].y <= UI_H - SKY_MARGIN);
            // The entire real constellation is translated, preserving its shape.
            assert(w.star[i].y - (UI_CY - 240) >= 84);
            assert(w.star[i].y - (UI_CY - 240) <= 396);
        }
    }
}
static void stream_lifetime_uses_visible_height(void) {
    sky_world_t w = {0}; sky_force_t force = {0}; sky_hits_t hits = {0};
    sky_particle_t point = {.x=UI_CX * SKY_Q3,.y=(UI_H + 10) * SKY_Q3};
    sky_stream_step(&w, &point, 1, SKY_STEP_S, &force, &hits);
    assert(!point.wait && point.age == 1);
    point.y = (UI_H + 31) * SKY_Q3;
    sky_stream_step(&w, &point, 1, SKY_STEP_S, &force, &hits);
    assert(point.wait == UINT16_MAX);
}
static void plush_origin_and_breathing(void) {
    fresh(); face_set_character(&face, CHARACTER_PLUSH);
    face.body.cur = -1;
    body_update(&face, 1.f / 30);
    assert(face.body_dy == FACE_Y_OFFSET);
    prim_t sprite = {.by0=100 + FACE_Y_OFFSET,.by1=479 + FACE_Y_OFFSET};
    sprite_breathe(&sprite, 1.012f);
    assert(sprite.by1 <= UI_H - 1);
    assert(sprite.by0 > 85 + FACE_Y_OFFSET && sprite.by0 < 100 + FACE_Y_OFFSET);
}
int main(void) {
    projection_keeps_equal_axes();
    scatter_uses_the_full_height();
    falling_points_reach_both_edges();
    body_rim_allows_portrait_travel();
    skies_bottom_touch_and_catalog();
    stream_lifetime_uses_visible_height();
    plush_origin_and_breathing();
    printf("world geometry: %dx%d, uniform projection, scatter, floor, rim, Skies and Plush passed\n", UI_W, UI_H);
    return 0;
}
