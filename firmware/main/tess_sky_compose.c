#include "tess_sky_internal.h"

#include <math.h>
#include <string.h>

// Compose a playable piece of sky, not a collection of independent obstacles. The same stream used in play
// proves a reachable tilt or finger route. Stars lie along that route, and at least one is missed without input.
// Work is sliced across frames; a failed candidate advances the seed within a fixed budget.
#define PI_F 3.14159265f
#define ATTEMPTS 8
#define TRACE_STEPS 480
#define TRACE_EVERY 5
#define MIN_STAR_GAP 80.f
#define VERIFY_STEPS 300

typedef struct { float x, y; } point_t;
enum { ST_LAYOUT, ST_VERIFY, ST_IDLE };
static const uint8_t semitones[5] = {0, 2, 4, 7, 9};
float sky_degree_hz(int degree) {
    if (degree < 0) degree = 0;
    return 293.665f * powf(2.f, (12.f * (float)(degree / 5) + semitones[degree % 5]) / 12.f);
}
static point_t at(const sky_path_t *p, int i) { return (point_t){p->x[i] / (float)SKY_Q3, p->y[i] / (float)SKY_Q3}; }
static bool in_box(float x, float y, float margin) {
    return x >= margin && x <= SKY_W - margin && y >= margin + 12.f && y <= SKY_W - margin;
}
static float path_gap(const sky_path_t *p, float x, float y) {
    float best = 1e9f;
    for (int i = 0; i < p->n; i++) best = fminf(best, hypotf(at(p, i).x - x, at(p, i).y - y));
    return best;
}
static bool star_fits(const sky_world_t *w, float x, float y) {
    if (!in_box(x, y, SKY_MARGIN + 6.f) || hypotf(x - w->source.x, y - w->source.y) < 74.f) return false;
    for (int i = 0; i < w->count_stars; i++)
        if (hypotf(w->star[i].x - x, w->star[i].y - y) < MIN_STAR_GAP) return false;
    for (int i = 0; i < w->count_holes; i++)
        if (hypotf(w->hole[i].x - x, w->hole[i].y - y) < 68.f) return false;
    return true;
}
static void place_star(sky_world_t *w, float x, float y) {
    sky_star_t *s = &w->star[w->count_stars++];
    *s = (sky_star_t){.x = x, .y = y, .ax = x, .ay = y};
}
static bool place_first(sky_world_t *w) {
    sky_force_t none = {0};
    sky_path_t path;
    sky_trace(w, &none, TRACE_STEPS, TRACE_EVERY, &path);
    int start = path.n / 3;
    for (int j = start; j < path.n; j++) {
        point_t p = at(&path, j);
        float side = (sky_rand(w) & 1) ? 1.f : -1.f;
        float off = sky_range(w, 72.f, 88.f);
        float x = p.x - w->source.dy * side * off, y = p.y + w->source.dx * side * off;
        if (!star_fits(w, x, y)) continue;
        place_star(w, x, y);
        w->touch_solution = true;
        return true;
    }
    return false;
}
static void place_holes(sky_world_t *w, int count) {
    const sky_source_t *s = &w->source;
    for (int n = 0; n < count; n++) {
        for (int tries = 0; tries < 12; tries++) {
            float side = sky_rand(w) & 1 ? 1.f : -1.f;
            float along = sky_range(w, 105.f + 70.f * n, 185.f + 70.f * n), off = sky_range(w, 42.f, 90.f);
            float x = s->x + s->dx * along - s->dy * off * side, y = s->y + s->dy * along + s->dx * off * side;
            if (!in_box(x, y, 60.f)) continue;
            if (n && hypotf(w->hole[0].x - x, w->hole[0].y - y) < 114.f) continue;
            w->hole[w->count_holes++] = (sky_hole_t){.x = x, .y = y, .r = sky_range(w, 112.f, 150.f), .spin = (int8_t)side};
            break;
        }
    }
}
static bool scatter_stars(sky_world_t *w, int count) {
    sky_force_t force;
    sky_solution(w, &force);
    sky_path_t path;
    sky_trace(w, &force, TRACE_STEPS, TRACE_EVERY, &path);
    int order[SKY_PATH_N];
    for (int i = 0; i < path.n; i++) order[i] = i;
    for (int i = path.n - 1; i > 0; i--) {
        int j = sky_pick(w, 0, i), swap = order[j]; order[j] = order[i]; order[i] = swap;
    }
    for (int i = 0; i < path.n && w->count_stars < count; i++) {
        point_t p = at(&path, order[i]);
        if (star_fits(w, p.x, p.y)) place_star(w, p.x, p.y);
    }
    return w->count_stars >= (count > 2 ? count - 1 : count);
}
static bool misses_without_input(const sky_world_t *w) {
    sky_force_t none = {0};
    sky_path_t path;
    sky_trace(w, &none, TRACE_STEPS, TRACE_EVERY, &path);
    for (int i = 0; i < w->count_stars; i++)
        if (path_gap(&path, w->star[i].ax, w->star[i].ay) > SKY_ZONE_R + 30.f) return true;
    return false;
}
static void tune(sky_world_t *w, int root, int drifting) {
    static const uint8_t ladder[SKY_STARS] = {0, 2, 4, 5, 7, 9};
    w->root = (uint8_t)root;
    for (int i = 0; i < w->count_stars; i++) {
        sky_star_t *s = &w->star[i];
        int degree = root + ladder[i];
        while (degree > 9) degree -= 5;
        s->degree = (uint8_t)degree; s->hz = sky_degree_hz(degree);
        s->color = (uint8_t)((i + root) % COL_COUNT);
        if (i >= drifting) continue;
        s->orbit = sky_range(w, 10.f, 16.f);
        s->spin = sky_range(w, .25f, .48f) * (sky_rand(w) & 1 ? 1.f : -1.f);
        s->phase = sky_range(w, 0, 2.f * PI_F);
    }
}
static void prepare_probe(sky_composer_t *c, int stage) {
    c->probe = *c->w;
    c->probe.feed_scale = (float)SKY_PARTICLES / SKY_PROBE_N;
    sky_stream_launch(&c->probe, c->light, SKY_PROBE_N, NULL);
    c->steps = c->tries = 0;
    c->stage = (uint8_t)stage;
}
static bool layout(sky_composer_t *c) {
    sky_world_t *w = c->w;
    memset(w, 0, sizeof *w);
    w->rng = c->seed * 2654435761u + c->attempt * 40503u + 1u;
    w->feed_scale = 1.f;
    sky_choose_plan(w, &c->r, &c->pl);
    sky_place_source(w, &c->r, c->pl.pour);
    if (c->r.aurora) {
        place_holes(w, c->pl.holes);
        tune(w, c->r.root, 0);
        return w->count_holes == c->pl.holes;
    }
    if (c->r.first) {
        if (!place_first(w)) return false;
    } else {
        float side = sky_rand(w) & 1 ? 1.f : -1.f;
        float push = c->pl.pour ? sky_range(w, .32f, .52f) : sky_range(w, .20f, .29f);
        w->gravity[0] = -w->source.dy * push * side; w->gravity[1] = w->source.dx * push * side;
        place_holes(w, c->pl.holes);
        if (w->count_holes < c->pl.holes || !scatter_stars(w, c->pl.stars)) return false;
    }
    if (!misses_without_input(w)) return false;
    tune(w, c->r.root, c->pl.drift);
    prepare_probe(c, ST_VERIFY);
    return true;
}
static point_t rotate(float x, float y, float dx, float dy) {
    return (point_t){SKY_CX + dx * x - dy * y, SKY_CY + dy * x + dx * y};
}
// A simpler finger sky ends the bounded search. Its rotated family is checked independently by the host matrix,
// including the no-input run. Later lessons retain their black holes and a second note instead of restarting lesson 1.
static void fallback(sky_composer_t *c) {
    sky_world_t *w = c->w;
    uint32_t seed = c->seed * 2654435761u + 977u;
    memset(w, 0, sizeof *w);
    w->rng = seed; w->feed_scale = 1; w->touch_solution = !c->r.aurora;
    sky_choose_plan(w, &c->r, &c->pl);
    float a = sky_range(w, 0, 2.f * PI_F), dx = cosf(a), dy = sinf(a);
    w->source = (sky_source_t){.x = SKY_CX - dx * 150, .y = SKY_CY - dy * 150, .dx = dx, .dy = dy, .speed = 120};
    for (int i = 0; i < c->pl.holes; i++) {
        point_t p = rotate(i ? 110 : -40, i ? -95 : -120, dx, dy);
        w->hole[w->count_holes++] = (sky_hole_t){.x=p.x,.y=p.y,.r=112,.spin=i ? -1 : 1};
    }
    if (!c->r.aurora) {
        point_t p = rotate(20, 76, dx, dy);
        place_star(w, p.x, p.y);
        if (c->pl.stars > 1) { p = rotate(-70, 0, dx, dy); place_star(w, p.x, p.y); }
    }
    tune(w, c->r.root, 0);
    c->simple = true;
}
static bool failed(sky_composer_t *c) {
    c->stage = ST_LAYOUT;
    c->attempt++;
    return false;
}
static bool verify_slice(sky_composer_t *c, unsigned steps) {
    sky_force_t force = {0};
    if (c->stage == ST_VERIFY) sky_solution(c->w, &force);
    for (unsigned n = 0; n < steps; n++) {
        sky_stream_step(&c->probe, c->light, SKY_PROBE_N, 1.f / 30.f, &force, NULL);
        c->steps++;
        if (sky_solved(&c->probe)) {
            if (c->stage == ST_IDLE) return failed(c);
            prepare_probe(c, ST_IDLE);
            return false;
        }
        if (c->steps < (c->stage == ST_IDLE ? 450 : VERIFY_STEPS)) continue;
        return c->stage == ST_IDLE ? true : failed(c);
    }
    return false;
}
void sky_compose_begin(sky_composer_t *c, sky_world_t *w, uint32_t seed, const sky_recipe_t *r) {
    memset(c, 0, sizeof *c);
    c->w = w; c->seed = seed; c->r = *r;
}
bool sky_compose_step(sky_composer_t *c, unsigned steps) {
    if (c->attempt >= ATTEMPTS) { fallback(c); return true; }
    if (c->stage != ST_LAYOUT) return verify_slice(c, steps);
    return layout(c) ? c->r.aurora : failed(c);
}
void sky_compose(sky_world_t *w, uint32_t seed, const sky_recipe_t *r) {
    sky_composer_t c;
    sky_compose_begin(&c, w, seed, r);
    while (!sky_compose_step(&c, SKY_COMPOSE_STEPS)) {}
}
