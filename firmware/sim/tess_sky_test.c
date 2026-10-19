#include "tess_sky_internal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void successful_streams_exist(const sky_world_t *w, const sky_particle_t *p) {
    unsigned owners=0, counts[SKY_CHANNELS]={0};
    for(int i=0;i<SKY_PARTICLES;i++)if(p[i].channel) {
        assert(p[i].channel<=w->channels);
        owners|=1u<<(p[i].channel-1);counts[p[i].channel-1]++;
    }
    assert(owners==(1u<<w->channels)-1);
    for(int i=0;i<w->channels;i++)assert(counts[i]<=SKY_CHANNEL_DOTS);
}
static int solved_by(sky_world_t w, bool guide) {
    sky_particle_t p[SKY_PARTICLES];
    sky_force_t force = {0};
    sky_stream_launch(&w, p, SKY_PARTICLES, NULL);
    float held = 0;
    for (int i = 0; i < (guide ? 900 : 450); i++) {
        if (guide && i % 4 == 0) sky_solution(&w, &force); // a normal 120–130 ms input lease
        assert(force.count <= 1 && hypotf(force.gx, force.gy) <= .301f);
        sky_stream_step(&w, p, SKY_PARTICLES, 1.f / 30, &force, NULL);
        successful_streams_exist(&w,p);
        held = sky_solved(&w) ? held + 1.f / 30 : 0;
        if (held >= SKY_HOLD_S) return i;
    }
    return -1;
}
static void validate_edges(const sky_world_t *w) {
    const sky_edge_t *edges;
    int count = sky_constellation_edges(w, &edges);
    if (!w->chart) { assert(!count && !sky_constellation_name(w)); return; }
    assert(count >= 2 && count <= 8 && sky_constellation_name(w));
    unsigned reached = 0;
    for (int i = 0; i < count; i++) {
        int a = edges[i].a, b = edges[i].b;
        assert(a < w->count_stars && b < w->count_stars && a != b);
        reached |= (1u << a) | (1u << b);
        for (int j = 0; j < i; j++) assert(!((edges[j].a == a && edges[j].b == b) || (edges[j].a == b && edges[j].b == a)));
    }
    assert(reached == (1u << w->count_stars) - 1);
}
static void validate(const sky_world_t *w) {
    assert(w->count_stars <= SKY_STARS && w->count_holes <= SKY_HOLES);
    assert(w->count_stars >= 1);
    for (int i = 0; i < w->count_stars; i++) {
        const sky_star_t *s = &w->star[i];
        assert(s->fill == (i ? 0 : 1) && s->sang == (i == 0) && isfinite(s->hz) && s->orbit == 0);
        assert(s->x >= SKY_MARGIN && s->x <= SKY_W - SKY_MARGIN);
        assert(s->y >= SKY_MARGIN && s->y <= SKY_W - SKY_MARGIN);
        assert(s->x == s->ax && s->y == s->ay);
        for (int j = 0; j < i; j++) assert(hypotf(s->x - w->star[j].x, s->y - w->star[j].y) > 12.f);
    }
    validate_edges(w);
}

static void progression(void) {
    static const char *const names[] = {"Triangulum", "Crux", "Corvus", "Delphinus", "Cassiopeia", "Cepheus", "Ursa Minor", "Orion"};
    static const int counts[] = {3,4,4,5,5,5,7,7};
    for (int i = 0; i < SKY_CHARTS; i++) {
        sky_recipe_t r = {.elements=(uint8_t)(i+1)};
        sky_world_t w; sky_compose(&w, 1, &r);
        assert(w.chart == i+1 && w.count_stars == counts[i]);
        assert(!strcmp(sky_constellation_name(&w), names[i]));
    }
    for (int solved = 9; solved < 25; solved++) {
        sky_recipe_t r = {.elements=10,.solved=(uint16_t)solved};
        sky_world_t w; sky_compose(&w, 42, &r);
        assert(!strcmp(sky_constellation_name(&w), names[solved%SKY_CHARTS]));
    }
}
static void completed_route_holds_the_final_touch_without_changing_progress(void) {
    sky_world_t w; sky_recipe_t r={.elements=9}; sky_compose(&w,42,&r);
    for(int i=0;i<w.count_stars;i++)w.star[i].fill=1;
    sky_world_t before=w; sky_force_t force; sky_solution(&w,&force);
    assert(force.count==1);
    assert(hypotf(force.touch[0].x-w.star[w.count_stars-1].x,force.touch[0].y-w.star[w.count_stars-1].y)<=40.1f);
    assert(!memcmp(&w,&before,sizeof w));
}
static void free_skies_have_no_targets(void) {
    for (int seed = 1; seed <= 32; seed++) {
        sky_recipe_t r = {.elements=10,.aurora=true,.root=(uint8_t)(seed%5)};
        sky_world_t w; sky_compose(&w, (uint32_t)seed, &r); validate(&w);
        assert(w.kind == SKY_AURORA && w.count_holes == 2 && !sky_solved(&w));
        assert(w.count_stars==1 && w.star[0].fill==1 && w.source.x==w.star[0].x && w.source.y==w.star[0].y);
        sky_particle_t p[SKY_PARTICLES]; sky_stream_launch(&w, p, SKY_PARTICLES, NULL);
        unsigned colours = 0; for (int i=0; i<SKY_PARTICLES; i++) colours |= 1u << p[i].color;
        assert(colours == 7);
    }
}
int main(int argc, char **argv) {
    int seeds = argc > 1 ? atoi(argv[1]) : 40, total=0, failures=0, passive=0;
    unsigned turns[SKY_CHARTS+1] = {0};
    for (int root=0; root<5; root++) for (int lesson=1; lesson<=SKY_LESSONS; lesson++) for (int seed=1; seed<=seeds; seed++) {
        sky_recipe_t r = {.elements=(uint8_t)lesson,.root=(uint8_t)root,.solved=(uint16_t)(lesson-1)};
        sky_world_t a,b; uint32_t key = (uint32_t)(seed*977+lesson*43+root);
        sky_compose(&a,key,&r); sky_compose(&b,key,&r);
        assert(!memcmp(&a,&b,sizeof a)); validate(&a); total++;
        turns[a.chart] |= 1u << a.turn;
        int result = solved_by(a,true), idle = solved_by(a,false);
        if (result<0 || idle>=0) printf("chart=%u turn=%u seed=%u solution=%d passive=%d\n",a.chart,a.turn,key,result,idle);
        failures += result<0; passive += idle>=0;
    }
    for (int i=1; i<=SKY_CHARTS; i++) assert(turns[i]==15);
    printf("Skies: %d catalog relay worlds, all 4 rotations and 5 roots, %d unsolved, %d passive\n",total,failures,passive);
    assert(!failures && !passive);
    progression(); completed_route_holds_the_final_touch_without_changing_progress(); free_skies_have_no_targets();
    return 0;
}
