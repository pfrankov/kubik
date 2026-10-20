// Production Skies rendering: telescope focus, bounded scenes, invisible gravity, fades and constellation tours.
// Native RGB565 properties are portable; no platform-specific image hashes or copied raster implementation.
#include "../main/tess_sky_play.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static face_t face;
static tess_sky_t sky;
static scene_t scene;
static uint16_t frame[R_H][R_W];

static void fixture(void) {
    face_init(&face);
    face_set_character(&face, CHARACTER_TESS);
    face_set_mode(&face, MODE_IDLE);
    face.boot_t = face.t = 10;
    face.tess_games.ready = true;
    face.tess_games.form = 3;
    face.tess_games.game = TESS_GAME_SKY;
    face.tess_games.sky = &sky;
    memset(&sky, 0, sizeof sky);
    sky.phase = SKY_PLAY; sky.w.t = 6; sky.seed = 732; sky.hint_star = -1;
    sky.w.source = (sky_source_t){.x = 46, .y = 284, .dx = 1};
    sky.w.count_holes = SKY_HOLES;
    sky.w.hole[0] = (sky_hole_t){.x = 208, .y = 164, .r = 110, .spin = 1};
    sky.w.hole[1] = (sky_hole_t){.x = 276, .y = 330, .r = 104, .spin = -1};
    sky.touch[0] = (sky_touch_t){.x = 122, .y = 330, .shown = 1, .spin = 1, .down = true};
    sky.touch[1] = (sky_touch_t){.x = 374, .y = 150, .shown = 1, .spin = -1, .down = true};
    sky.w.count_stars = SKY_STARS;
    const float places[SKY_STARS][2] = {{95,85},{384,64},{418,270},{370,406},{160,418},{59,240},{240,180}};
    for (int i = 0; i < SKY_STARS; i++)
        sky.w.star[i] = (sky_star_t){.x = places[i][0], .y = places[i][1], .fill = 1, .glow = 1, .color = i % COL_COUNT};
    for (int i = 0; i < SKY_PARTICLES; i++)
        sky.p[i] = (sky_particle_t){.x = (int16_t)((40 + i % 14 * 30) * SKY_Q3),
                                   .y = (int16_t)((60 + i / 14 * 48) * SKY_Q3), .vx = 1200, .vy = 480, .color = i % COL_COUNT};
}

static void particles(void) {
    for (int i = 0; i < SKY_PARTICLES; i++)
        sc_glow_dot_q3(&scene, sky.p[i].x, sky.p[i].y, 12, 0x9AF9E0, 60);
}

static void capacity_at(float fill, float glow, bool resolve) {
    fixture();
    if(resolve) {
        sky_recipe_t recipe={.elements=8};sky_compose(&sky.w,4,&recipe);sky.w.t=6;
        sky.phase=SKY_RESOLVE;sky.resolve_t=4;sky.resolve_len=8;
        sky.melody.n=SKY_STARS;sky.melody.unit_ms=160;
        for(int i=0;i<SKY_STARS;i++)sky.melody.note[i]=(sky_note_t){.star=(int8_t)i,.units=1};
    }
    for (int i = 0; i < SKY_STARS; i++) { sky.w.star[i].fill = fill; sky.w.star[i].glow = glow; }
    // The larger native primitive pool reveals overflow before the device's 64-slot builder can hide it.
    assert(R_MAX_PRIMS == 80);
    scene_begin(&scene, 0);
    particles();
    // Reserve room for a point-stage trail and normal home overlays, even with every full star flashing.
    for (int i = 0; i < 5; i++) sc_glow_dot(&scene, 5.f + i * 5.f, 8, 1, 0xFFFFFF, 0);
    tess_sky_draw(&face, &scene);
    assert(scene.n + 16 < 64);
    assert(scene.dots_n < R_MAX_DOTS); // Equality could conceal a silently discarded dot.
    unsigned game_prims = (unsigned)scene.n, game_dots = (unsigned)scene.dots_n;
    face_set_power(&face, true, 15, true, true);
    face.cron_running = 2; face.cron_due = 3; face.cron_k = 1;
    face_draw(&face, &scene);
    assert(scene.n < 64 && scene.dots_n < R_MAX_DOTS);
    printf("Skies render capacity at %.2f: game %u prims + 16 reserved, %u dots; full home %d prims, %d dots\n",
           fill, game_prims, game_dots, scene.n, scene.dots_n);
}

static void capacity(void) {
    capacity_at(0, 0, false);
    capacity_at(SKY_FULL * .5f, 0, false); // Blurred discs must fit without filling the primitive or dot pool.
    capacity_at(1, 1, false);
    capacity_at(1, 1, true);
}

static void push(int x0, int y0, int x1, int y1, uint16_t *p, void *context) {
    (void)context;
    for (int y = y0; y < y1; y++) memcpy(frame[y] + x0, p + (y - y0) * (x1 - x0), (size_t)(x1 - x0) * 2);
}

static void raster(void) {
    uint16_t b0[R_W * R_BAND], b1[R_W * R_BAND], *buffers[2] = {b0, b1};
    render_state_t state = {0};
    render_frame(&state, &scene, buffers, 2, false, push, NULL);
}

typedef struct { unsigned core; float spread, split; } star_light_t;

static star_light_t star_light_size(float fill, int color, float time, int size) {
    sky.w.star[0] = (sky_star_t){.x=240, .y=240, .ax=240, .ay=240, .fill=fill, .color=(uint8_t)color, .size=(int8_t)size};
    face.t = time;
    scene_begin(&scene, 0); tess_sky_draw(&face, &scene); raster();
    unsigned core = 0, total = 0, moment = 0, red=0, blue=0;
    int red_x=0, blue_x=0;
    for (int y = 96; y < 144; y++) {
        for (int x = 96; x < 144; x++) {
            uint16_t c = frame[y][x];
            unsigned energy = ((c >> 11) & 31) + ((c >> 6) & 31) + (c & 31);
            int dx = x * 2 + 1 - 240, dy = y * 2 + 1 - 240, radius2 = dx * dx + dy * dy;
            unsigned r=(c>>11)&31, b=c&31; red+=r;blue+=b;red_x+=(int)r*dx;blue_x+=(int)b*dx;
            total += energy; moment += energy * (unsigned)radius2;
            if (radius2 < 36) core += energy;
        }
    }
    assert(total > 0);
    return (star_light_t){core, (float)moment / (float)total, fabsf((float)red_x/red-(float)blue_x/blue)};
}

static star_light_t star_light(float fill, int color, float time) {
    return star_light_size(fill,color,time,0);
}

static void star_sizes_follow_the_catalog(void) {
    fixture();sky.w.count_stars=1;
    for(int i=0;i<SKY_PARTICLES;i++)sky.p[i].wait=1;
    star_light_t small=star_light_size(0,0,6,-20),large=star_light_size(0,0,6,20);
    assert(large.spread>small.spread*1.4f);
    for(int chart=1;chart<=SKY_CHARTS;chart++) {
        sky_recipe_t r={.elements=(uint8_t)chart}; sky_world_t world;
        sky_compose(&world,42,&r);
        int low=100,high=-100;
        for(int i=0;i<world.count_stars;i++) {
            int size=world.star[i].size; assert(size>=-20 && size<=20);
            if(size<low)low=size; if(size>high)high=size;
        }
        assert(high>low);
    }
}

static void stars_gather_light(void) {
    fixture();
    sky.w.count_stars = 1;
    sky.w.source.x = sky.w.source.y = -100;
    for (int i = 0; i < SKY_PARTICLES; i++) sky.p[i].wait = 1;
    for (int c = 0; c < COL_COUNT; c++) {
        for (int t = 0; t < 8; t++) {
            float time = 6.f + (float)t * 1.7f;
            star_light_t empty = star_light(0, c, time), half = star_light(SKY_FULL * .5f, c, time);
            star_light_t near = star_light(SKY_FULL * .75f, c, time), full = star_light(1, c, time);
            assert(empty.split > .25f && empty.split > half.split && half.split > full.split);
            // Native pixels must concentrate around a brighter core, instead of filling an outer progress ring.
            assert(empty.spread > half.spread * 1.25f && half.spread > near.spread * 1.25f);
            assert(empty.core < half.core && half.core < near.core && near.core < full.core);
            // Draining returns to the same loose formation. Drawing never carries its own progress forward.
            star_light_t drained = star_light(SKY_FULL * .5f, c, time);
            assert(drained.core == half.core && drained.spread == half.spread);
        }
    }
    (void)star_light(SKY_FULL * .5f, COL_TEAL, 6.f);
    scene_begin(&scene, 0); tess_sky_draw(&face, &scene);
    scene_t unhinted = scene;
    sky.hint_star = 0;
    sky_world_t before = sky.w;
    scene_begin(&scene, 0); tess_sky_draw(&face, &scene);
    // A hint may brighten the light, but must not pretend to fill a star or advance the world/random stream.
    assert(memcmp(&before, &sky.w, sizeof before) == 0 && scene.dots_n == unhinted.dots_n);
    for (int i = 0; i < scene.dots_n; i++) {
        assert(scene.dots[i].x == unhinted.dots[i].x && scene.dots[i].y == unhinted.dots[i].y);
        assert(scene.dots[i].radius == unhinted.dots[i].radius);
    }
}

static void render_sky(void) {
    // A bright background exposes even a black mask that could hide on the normal dark sky.
    scene_begin(&scene, rgb565(0x63BDC6));
    particles();
    tess_sky_draw(&face, &scene);
    raster();
}

static void compare_without_fields(void) {
    static uint16_t with_fields[R_H][R_W];
    sky_touch_t touches[SKY_TOUCHES];
    memcpy(touches, sky.touch, sizeof touches);
    uint8_t count = sky.w.count_holes;
    render_sky();
    memcpy(with_fields, frame, sizeof frame);
    sky.w.count_holes = 0; memset(sky.touch, 0, sizeof sky.touch);
    render_sky();
    assert(frame[R_H - 1][R_W - 1] != 0);
    // Freeze the stream: adding gravity must add no pixels or occlude any existing light.
    assert(memcmp(with_fields, frame, sizeof frame) == 0);
    sky.w.count_holes = count; memcpy(sky.touch, touches, sizeof touches);
}

static void holes_leave_no_visual_trace(void) {
    fixture();
    for (int phase = SKY_PLAY; phase <= SKY_FADE; phase++) {
        sky.phase = (uint8_t)phase; sky.resolve_len = 8;
        compare_without_fields();
    }
    sky.phase = SKY_PLAY;
    // Overlapping fields, glass corners and release must stay invisible too.
    sky.touch[0].x = sky.w.hole[0].x; sky.touch[0].y = sky.w.hole[0].y - 26;
    compare_without_fields();
    sky.touch[0].x = sky.touch[0].y = 0;
    sky.touch[1].x = sky.touch[1].y = 479;
    compare_without_fields();
    sky.touch[0].down = sky.touch[1].down = false;
    sky.touch[0].shown = sky.touch[1].shown = .5f;
    compare_without_fields();
}

static void sky_effects_fade_completely(void) {
    fixture();
    sky.fade = SKY_FADE_S;
    scene_begin(&scene, 0); tess_sky_draw(&face, &scene);
    assert(scene.n == 0 && scene.dots_n == 0);
    sky.fade = 0; sky.w.t = 0;
    scene_begin(&scene, 0); tess_sky_draw(&face, &scene);
    assert(scene.n == 0 && scene.dots_n == 0);
    sky.w.t = 6; sky.phase = SKY_UNRAVEL;
    scene_begin(&scene, 0); tess_sky_draw(&face, &scene);
    assert(scene.n == 0 && scene.dots_n == 0);
}

static void freeplay_aurora_needs_no_targets(void) {
    fixture();
    sky_recipe_t recipe={.elements=8,.aurora=true};sky_compose(&sky.w,4,&recipe);sky.w.t=6;
    assert(sky.w.count_stars==1 && sky.w.star[0].fill==1);
    for (int phase = SKY_PLAY; phase <= SKY_RESOLVE; phase++) {
        sky.phase = (uint8_t)phase; sky.resolve_len = 8;
        scene_begin(&scene, 0); particles(); tess_sky_draw(&face, &scene); raster();
        assert(scene.n == 2 && scene.dots_n > SKY_PARTICLES && scene.dots_n < R_MAX_DOTS);
        compare_without_fields();
    }
}

static void composing_next_sky_does_not_change_current_sky(void) {
    fixture();
    scene_begin(&scene, 0); tess_sky_draw(&face, &scene);
    raster();
    static uint16_t previous[R_H][R_W];
    memcpy(previous, frame, sizeof frame);
    sky.seed ^= 0x93ED6B4Fu; // The composer owns this seed before the previous sky's melody has ended.
    scene_begin(&scene, 0); tess_sky_draw(&face, &scene);
    raster();
    assert(memcmp(previous, frame, sizeof frame) == 0);
}

static bool middle_of_link(const sky_star_t *a, const sky_star_t *b, float from, float until) {
    float dx = b->x - a->x, dy = b->y - a->y, length2 = dx * dx + dy * dy;
    for (int i = 0; i < scene.dots_n; i++) {
        float x = scene.dots[i].x / 8.f - a->x, y = scene.dots[i].y / 8.f - a->y;
        float along = (x * dx + y * dy) / length2, cross = x * dy - y * dx;
        if (along > from && along < until && cross * cross < length2) return true;
    }
    return false;
}

static void assert_name(const char *name) {
        int labels=0;
        for (int i=0; i<scene.n; i++) if (scene.p[i].kind==PK_TEXT) {
            const prim_t *p=&scene.p[i]; labels++;
            assert(p->b==(int)strlen(name) && p->bx0>=24 && p->bx1<=456 && p->by0>=424 && p->by1<=468);
            for (int j=0;j<p->b;j++) assert(scene.text[p->a+j]==font_glyph(&g_fonts[p->r],name[j]));
        }
        assert(labels==1);
}

static void real_constellation_links_and_success_names(void) {
    for (int id=1; id<=SKY_CHARTS; id++) {
        fixture();
        sky_recipe_t recipe = {.elements=(uint8_t)id};
        sky_compose(&sky.w, 42, &recipe); sky.w.t=6;
        const char *name = sky_constellation_name(&sky.w);
        assert(name);
        scene_begin(&scene,0); tess_sky_draw(&face,&scene);
        for (int i=0; i<scene.n; i++) assert(scene.p[i].kind!=PK_TEXT); // no label before success
        sky.phase=SKY_RESOLVE; sky.resolve_t=3; sky.resolve_len=8;
        sky.melody.n=sky.w.count_stars; sky.melody.unit_ms=200;
        for (int i=0; i<sky.w.count_stars; i++) {
            sky.w.star[i].fill=1;
            int star=sky.w.count_stars-1-i; // music order must not rewire the figure
            sky.melody.tour[i]=(int8_t)star;
            sky.melody.note[i]=(sky_note_t){.star=(int8_t)star,.units=1};
        }
        scene_begin(&scene,0); tess_sky_draw(&face,&scene);
        assert(scene.dots_n<R_MAX_DOTS && scene.n<64);
        const sky_edge_t *edges; int count=sky_constellation_edges(&sky.w,&edges);
        for (int i=0; i<count; i++)
            assert(middle_of_link(&sky.w.star[edges[i].a],&sky.w.star[edges[i].b],.1f,.9f));
        assert_name(name);
        sky.fade=SKY_FADE_S; scene_begin(&scene,0); tess_sky_draw(&face,&scene);
        assert(scene.n==0 && scene.dots_n==0); // title fades with the chart
    }
}

int main(void) {
    star_sizes_follow_the_catalog();
    capacity();
    stars_gather_light();
    holes_leave_no_visual_trace();
    sky_effects_fade_completely();
    freeplay_aurora_needs_no_targets();
    composing_next_sky_does_not_change_current_sky();
    real_constellation_links_and_success_names();
    puts("Skies render: telescope focus, bounded scenes, invisible fields, fades and catalog lines and success names OK");
}
