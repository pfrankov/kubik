// The real Skies stream: bounded gravity, independent touches, persistent progress and time-based lifetimes.
#include "tess_sky_internal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static sky_world_t world(void) {
    return (sky_world_t){.feed_scale = 1, .rng = 42,
        .source = {.x = 80, .y = 180, .dx = 1, .speed = 120}};
}
static sky_particle_t light(float x, float y, float vx, float vy) {
    return (sky_particle_t){.x = (int16_t)(x * SKY_Q3), .y = (int16_t)(y * SKY_Q3),
        .vx = (int16_t)(vx * SKY_Q3), .vy = (int16_t)(vy * SKY_Q3)};
}
static sky_touch_t touch(float x, float y) { return (sky_touch_t){.x=x,.y=y,.shown=1,.spin=1,.down=true}; }
static void motion_preserves_fractional_steps(void) {
    sky_world_t w = world();
    sky_particle_t p = light(220, 220, 113, -117);
    sky_force_t none = {0};
    for (int i = 0; i < 60; i++) sky_stream_step(&w, &p, 1, SKY_STEP_S, &none, NULL);
    assert(p.x == (220 + 113) * SKY_Q3 && p.y == (220 - 117) * SKY_Q3);
    w = world(); p = light(100, 100, 0, 0);
    sky_force_t tilt = {.gx = .1f};
    for (int i = 0; i < 60; i++) sky_stream_step(&w, &p, 1, SKY_STEP_S, &tilt, NULL);
    assert(p.vx == 30 * SKY_Q3 && p.vy == 0);
}
static void holes_bend_without_stopping(void) {
    sky_world_t w = world(), plain = w;
    sky_particle_t p = light(160, 190, 120, 0), straight = p;
    sky_force_t force = {.count = 1, .touch = {touch(240, 265)}}, none = {0};
    for (int i = 0; i < 45; i++) {
        sky_stream_step(&w, &p, 1, SKY_STEP_S, &force, NULL);
        sky_stream_step(&plain, &straight, 1, SKY_STEP_S, &none, NULL);
    }
    assert(p.y > straight.y + 12 * SKY_Q3);
    assert(hypotf(p.vx, p.vy) > 110 * SKY_Q3);
    assert(hypotf(p.vx, p.vy) < 220 * SKY_Q3);
}
static void both_touches_contribute(void) {
    sky_world_t one = world(), two = one;
    sky_particle_t a = light(220, 240, 110, 0), b = a;
    sky_force_t f = {.count = 1, .touch = {touch(180, 280), touch(290, 180)}};
    sky_stream_step(&one, &a, 1, SKY_STEP_S, &f, NULL);
    f.count = 2;
    sky_stream_step(&two, &b, 1, SKY_STEP_S, &f, NULL);
    assert(a.vx != b.vx && a.vy != b.vy);
    // At zero strength a released field has exactly no influence, even with a stale down bit.
    two = world(); b = light(220, 240, 110, 0); f.touch[1].shown = 0;
    sky_stream_step(&two, &b, 1, SKY_STEP_S, &f, NULL);
    assert(a.vx == b.vx && a.vy == b.vy);
}
static void the_same_hole_has_the_same_force(void) {
    sky_world_t a = world(), b = a;
    a.count_holes = 1;
    a.hole[0] = (sky_hole_t){.x=245,.y=270,.r=SKY_FINGER_R,.spin=-1};
    sky_particle_t pa = light(190,210,120,25), pb = pa;
    sky_force_t none = {0}, finger = {.count=1,.touch={touch(245,270)}};
    finger.touch[0].spin = -1;
    for (int i=0;i<90;i++) {
        sky_stream_step(&a,&pa,1,SKY_STEP_S,&none,NULL);
        sky_stream_step(&b,&pb,1,SKY_STEP_S,&finger,NULL);
    }
    assert(!memcmp(&pa,&pb,sizeof pa));
}
static void fields_remain_bounded(void) {
    sky_world_t w = world();
    w.count_holes=SKY_HOLES;
    for(int i=0;i<SKY_HOLES;i++) w.hole[i]=(sky_hole_t){.x=240,.y=240,.r=160,.spin=1};
    sky_force_t f = {.gx=1,.gy=1,.count=2,.touch={touch(240,240),touch(240,240)}};
    sky_particle_t p=light(240,240,330,330);
    for(int i=0;i<1200;i++) {
        sky_stream_step(&w,&p,1,SKY_STEP_S,&f,NULL);
        assert(hypotf(p.vx,p.vy) <= SKY_MAX_SPEED * SKY_Q3 + 1);
        assert(p.x > -4000 && p.y > -4000);
    }
}
static void stars_keep_their_progress(void) {
    sky_world_t w=world(); w.count_stars=1;
    w.star[0]=(sky_star_t){.x=240,.y=240,.fill=SKY_FULL-.0001f,.color=COL_GOLD};
    sky_particle_t p=light(240,240,0,0);
    sky_force_t none={0}; sky_hits_t hits;
    sky_stream_step(&w,&p,1,SKY_STEP_S,&none,&hits);
    assert(w.star[0].fill==1 && hits.locked==1 && w.star[0].sang);
    // No particles and a different visual colour cannot erase a note already earned.
    for(int i=0;i<600;i++) {
        sky_stream_step(&w,&p,0,SKY_STEP_S,&none,&hits);
        assert(w.star[0].fill==1 && hits.locked==0 && sky_solved(&w));
    }
    w.star[0].fill=.5f; w.star[0].sang=false;
    sky_stream_step(&w,&p,0,SKY_STEP_S,&none,&hits);
    assert(w.star[0].fill<.5f);
}
static void cadence_does_not_change_the_stream(void) {
    sky_world_t a=world(),b=a,c=a;
    a.count_holes=1; a.hole[0]=(sky_hole_t){.x=260,.y=260,.r=150,.spin=-1};
    a.count_stars=1; a.star[0]=(sky_star_t){.x=250,.y=205,.ax=250,.ay=205,.orbit=16,.spin=.48f};
    b=a;c=a;
    sky_particle_t pa[SKY_PARTICLES],pb[SKY_PARTICLES],pc[SKY_PARTICLES];
    sky_stream_launch(&a,pa,SKY_PARTICLES,NULL); b=a;c=a;
    memcpy(pb,pa,sizeof pa);memcpy(pc,pa,sizeof pa);
    sky_force_t f={.gx=.18f,.gy=.12f};
    for(int i=0;i<600;i++) {
        sky_stream_step(&a,pa,SKY_PARTICLES,1.f/30,&f,NULL);
        sky_stream_step(&b,pb,SKY_PARTICLES,1.f/60,&f,NULL);
        sky_stream_step(&b,pb,SKY_PARTICLES,1.f/60,&f,NULL);
    }
    for(int i=0;i<300;i++) {
        sky_stream_step(&c,pc,SKY_PARTICLES,.01f,&f,NULL);
        sky_stream_step(&c,pc,SKY_PARTICLES,1.f/15-.01f,&f,NULL);
    }
    assert(!memcmp(pa,pb,sizeof pa) && !memcmp(pa,pc,sizeof pa));
    assert(a.rng==b.rng && a.rng==c.rng && a.t==b.t && a.t==c.t);
    assert(!memcmp(a.star,b.star,sizeof a.star) && !memcmp(a.star,c.star,sizeof a.star));
}
int main(void) {
    motion_preserves_fractional_steps();
    holes_bend_without_stopping();both_touches_contribute();the_same_hole_has_the_same_force();
    fields_remain_bounded();stars_keep_their_progress();cadence_does_not_change_the_stream();
    puts("Skies gravity: fly-bys, two touches, speed bound, persistent notes and cadence passed");
}
