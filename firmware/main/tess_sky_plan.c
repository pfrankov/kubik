#include "tess_sky_internal.h"
#include <stddef.h>
#include <math.h>

// Measured star positions, conventional chart links and offline verified relay input routes.
// No per-star displacement: only the whole chart turns, preserving the real figure.
typedef struct { int16_t x, y; int8_t size; } chart_star_t;
typedef struct {
    const char *name;
    uint8_t stars, links, holes;
    chart_star_t star[SKY_STARS];
    sky_edge_t edge[8];
} chart_t;
#include "tess_sky_catalog.h"

static const chart_t *chart(const sky_world_t *w) {
    return w->chart > 0 && w->chart <= SKY_CHARTS ? &k_charts[w->chart - 1] : NULL;
}
static void turn(float *x, float *y, unsigned quarters) {
    for (unsigned i = 0; i < quarters; i++) { float old = *x; *x = -*y; *y = old; }
}
static void position(float x, float y, unsigned quarters, float *out_x, float *out_y) {
    x -= 240; y -= 240; turn(&x, &y, quarters); *out_x = x + 240; *out_y = y + 240;
}
const char *sky_constellation_name(const sky_world_t *w) {
    const chart_t *c = chart(w);
    return c ? c->name : NULL;
}
int sky_constellation_edges(const sky_world_t *w, const sky_edge_t **edges) {
    const chart_t *c = chart(w);
    *edges = c ? c->edge : NULL;
    return c ? c->links : 0;
}
static uint8_t choose_chart(const sky_recipe_t *r) {
    if (r->aurora) return 0;
    if (r->elements <= SKY_CHARTS) return r->elements ? r->elements : 1;
    return (uint8_t)(1 + r->solved % SKY_CHARTS);
}
static void place_holes(sky_world_t *w) {
    for (int i = 0; i < w->count_holes; i++) {
        sky_hole_t *h = &w->hole[i];
        position(i ? 390 : 90, i ? 390 : 90, w->turn, &h->x, &h->y);
        h->r = 112; h->spin = i ? -1 : 1;
    }
}
void sky_place_chart(sky_world_t *w, const sky_recipe_t *r) {
    w->chart = choose_chart(r);
    w->turn = (uint8_t)(sky_rand(w) & 3u);
    w->kind = r->aurora ? SKY_AURORA : r->elements == 1 ? SKY_POUR : SKY_PUZZLE;
    position(50, 240, w->turn, &w->source.x, &w->source.y);
    w->source.dx = 1; turn(&w->source.dx, &w->source.dy, w->turn);
    w->source.speed = 120;
    const chart_t *c = chart(w);
    w->count_stars = c ? c->stars : 1;
    w->count_holes = r->aurora ? SKY_HOLES : c ? c->holes : 0;
    for (int i = 0; i < w->count_stars; i++) {
        sky_star_t *s = &w->star[i];
        position(c ? c->star[i].x : 50, c ? c->star[i].y : 240, w->turn, &s->x, &s->y);
        s->ax = s->x; s->ay = s->y;
        s->size = c ? c->star[i].size : 0;
    }
    w->star[0].fill = 1; w->star[0].sang = true;
    if (c) sky_emitter(w, 0);
    place_holes(w);
}
// Diagnostics apply ordinary tilt and finger gravity only; no writes to source, fill or particles.
void sky_solution(const sky_world_t *w, sky_force_t *force) {
    *force = (sky_force_t){0};
    if (w->kind == SKY_AURORA) { force->gx = .06f; return; }
    int target = 0;
    while (target + 1 < w->count_stars && w->star[target].fill >= SKY_FULL) target++;
    if (!w->count_stars) return;
    float ox=0,oy=0;
    if (w->chart) {
        ox=k_route[w->chart-1][w->emitter][target][2];
        oy=k_route[w->chart-1][w->emitter][target][3];
        turn(&ox,&oy,w->turn);
        force->gx = k_route[w->chart-1][w->emitter][target][0] * .075f;
        force->gy = k_route[w->chart-1][w->emitter][target][1] * .075f;
        turn(&force->gx, &force->gy, w->turn);
    }
    force->count = 1;
    force->touch[0] = (sky_touch_t){.x=w->star[target].x+ox,.y=w->star[target].y+oy,.shown=1,.spin=1,.down=true};
}
