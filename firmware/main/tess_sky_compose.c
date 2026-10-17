#include "tess_sky_internal.h"

#include <math.h>
#include <string.h>

// Making a sky. A seed, what the player has met and how they are doing give a harmony and a layout; the layout is
// built backwards from its own solution so that it can always be solved:
//   1. the source and a plan (how many stars, which tools, how many hazards);
//   2. the tools go onto the path of one light traced from the source, each where that light has gone so far,
//      turning it by the angle the tool is given;
//   3. the stars go on the finished path, so the stream that follows it lights them all;
//   4. the hazards go away from the path, and the tools are scrambled until the path misses a star.
// A layout that does not come out is dropped and the seed advanced: at most ATTEMPTS tries, then the simplest sky.
#define ATTEMPTS 8
#define TRACE_STEPS 230
#define TRACE_EVERY 3
#define PI_F 3.14159265f
#define MIN_STAR_GAP 86.f
#define TOOL_GAP 66.f
#define STAR_TOOL_GAP 44.f   // a star this far from a string (its zone is 36)

typedef sky_plan_t plan_t;
typedef struct { float x, y; } pt_t;

static const uint8_t k_semitone[5] = {0, 2, 4, 7, 9};
float sky_degree_hz(int degree) {
    if (degree < 0) degree = 0;
    return 293.665f * powf(2.f, (12.f * (float)(degree / 5) + k_semitone[degree % 5]) / 12.f);
}

static bool in_box(float x, float y, float margin) {
    return x >= margin && x <= SKY_W - margin && y >= margin + 12.f && y <= SKY_W - margin;
}
static pt_t at(const sky_path_t *p, int i) { return (pt_t){p->x[i] / (float)SKY_Q3, p->y[i] / (float)SKY_Q3}; }
static float gap(pt_t a, pt_t b) { return hypotf(a.x - b.x, a.y - b.y); }
static int branches(const sky_world_t *w) {
    for (int i = 0; i < w->count_tools; i++) if (w->tool[i].kind == TOOL_SPLIT) return 2;
    return 1;
}
static void trace_all(const sky_world_t *w, int parity, sky_path_t *path) {
    sky_force_t force = {.gx = w->gravity[0], .gy = w->gravity[1]};
    sky_trace(w, parity, &force, TRACE_STEPS, TRACE_EVERY, path);
}

static bool chance(sky_world_t *w, float p) { return sky_randf(w) < p; }
static float angle_gap(float a, float b) { float d = fabsf(a - b); while (d > PI_F) d = fabsf(d - 2 * PI_F); return d; }

// ---------------------------------------------------------------- the tools along the path
static bool crowded(const sky_world_t *w, float x, float y) {
    for (int i = 0; i < w->count_tools; i++)
        if (hypotf(w->tool[i].x - x, w->tool[i].y - y) < TOOL_GAP) return true;
    for (int i = 0; i < w->count_stars; i++)
        if (hypotf(w->star[i].x - x, w->star[i].y - y) < TOOL_GAP + 20.f) return true;
    return hypotf(w->source.x - x, w->source.y - y) < 60.f;
}
// The point `arc` px along the path and the way the light is going there.
static bool locate(const sky_path_t *p, float arc, pt_t *where, pt_t *dir) {
    for (int i = 1; i + 1 < p->n; i++) {
        if (p->arc[i] < arc) continue;
        *where = at(p, i);
        pt_t a = at(p, i - 1), b = at(p, i + 1);
        float len = gap(a, b);
        if (len < 1.f) return false;
        *dir = (pt_t){(b.x - a.x) / len, (b.y - a.y) / len};
        return true;
    }
    return false;
}
static void set_solution(sky_tool_t *t) { t->sx = t->x; t->sy = t->y; t->sdx = t->dx; t->sdy = t->dy; }

static bool put_string(sky_world_t *w, sky_tool_t *t, pt_t at_, pt_t u) {
    float turn = (chance(w, .5f) ? 1.f : -1.f) * sky_range(w, .75f, 1.65f), half = sky_range(w, 44.f, 68.f);
    float c = cosf(turn), s = sinf(turn);
    pt_t out = {u.x * c - u.y * s, u.x * s + u.y * c};
    pt_t n = {out.x - u.x, out.y - u.y};
    float len = hypotf(n.x, n.y);
    if (len < .05f) return false;
    t->dx = -n.y / len * half; t->dy = n.x / len * half;
    t->x = at_.x; t->y = at_.y;
    return in_box(t->x + t->dx, t->y + t->dy, 6.f) && in_box(t->x - t->dx, t->y - t->dy, 6.f);
}
static bool put_tool(sky_world_t *w, sky_tool_t *t, pt_t at_, pt_t u) {
    t->movable = 1;
    switch (t->kind) {
    case TOOL_STRING: return put_string(w, t, at_, u);
    case TOOL_WELL: {
        float side = chance(w, .5f) ? 1.f : -1.f, off = sky_range(w, 78.f, 104.f);
        t->x = at_.x - u.y * side * off + u.x * 12.f; t->y = at_.y + u.x * side * off + u.y * 12.f;
        t->r = sky_range(w, 124.f, 150.f);
        return in_box(t->x, t->y, 30.f);
    }
    case TOOL_PRISM:
        t->x = at_.x; t->y = at_.y; t->r = 34.f;
        t->color = (uint8_t)(chance(w, .5f) ? COL_GOLD : COL_ROSE);
        return true;
    default:  // TOOL_SPLIT: across the stream
        t->x = at_.x; t->y = at_.y; t->dx = -u.y * 42.f; t->dy = u.x * 42.f;
        return in_box(t->x + t->dx, t->y + t->dy, 14.f) && in_box(t->x - t->dx, t->y - t->dy, 14.f);
    }
}
// How far the light goes before it leaves the glass (or dies), px.
static float path_room(const sky_path_t *path) {
    for (int i = 0; i < path->n; i++) if (!in_box(at(path, i).x, at(path, i).y, 56.f)) return path->arc[i];
    return path->n ? path->arc[path->n - 1] : 0;
}
static float room(const sky_world_t *w) {
    sky_path_t path;
    trace_all(w, 0, &path);
    return path_room(&path);
}
static bool bends(int kind) { return kind == TOOL_STRING || kind == TOOL_WELL || kind == TOOL_SPLIT || kind == SKY_SEQ_RELAY; }
static int color_at(const sky_path_t *p, float arc) {
    int i = 1;
    while (i + 1 < p->n && p->arc[i] < arc) i++;
    return p->color[i];
}
// A relay star where the light is, sending it off by 45..135 degrees (one of its eight ways).
static bool put_relay(sky_world_t *w, const sky_path_t *path, float arc, pt_t where, pt_t u) {
    if (w->count_stars >= SKY_STARS || !in_box(where.x, where.y, 70.f) || hypotf(w->source.x - where.x, w->source.y - where.y) < 80.f) return false;
    float heading = atan2f(u.y, u.x);
    int ways[SKY_DIRS], n = 0;
    for (int d = 0; d < SKY_DIRS; d++) {
        float turn = angle_gap((float)d * PI_F / 4, heading);
        if (turn > .7f && turn < 2.4f) ways[n++] = d;
    }
    sky_star_t *s = &w->star[w->count_stars++];
    memset(s, 0, sizeof *s);
    s->x = s->ax = where.x; s->y = s->ay = where.y;
    s->relay = true;
    s->dir = s->sdir = (uint8_t)ways[sky_rand(w) % (unsigned)n];
    s->color = (uint8_t)color_at(path, arc);
    return true;
}
// One tool at `want` px along the light: false if there is no room for it or for the light to go on after it.
static bool try_tool(sky_world_t *w, const plan_t *pl, int j, float want, const sky_path_t *path) {
    pt_t where, dir;
    if (!locate(path, want, &where, &dir) || !in_box(where.x, where.y, 54.f) || crowded(w, where.x, where.y)) return false;
    if (pl->seq[j] == SKY_SEQ_RELAY) {
        if (!put_relay(w, path, want, where, dir)) return false;
        if (room(w) >= want + 150.f) return true;
        w->count_stars--;
        return false;
    }
    sky_tool_t *t = &w->tool[w->count_tools];
    memset(t, 0, sizeof *t);
    t->kind = pl->seq[j];
    if (!put_tool(w, t, where, dir)) return false;
    set_solution(t);
    w->count_tools++;
    if (!bends(t->kind) || room(w) >= want + (j + 1 < pl->seq_n ? 80.f : 130.f)) return true;
    w->count_tools--;
    return false;
}
// ---------------------------------------------------------------- the stars
// How far p is from the segment a-b.
static float seg_gap(pt_t p, pt_t a, pt_t b) {
    float ex = b.x - a.x, ey = b.y - a.y, len2 = ex * ex + ey * ey;
    float u = len2 > 0 ? fminf(fmaxf(((p.x - a.x) * ex + (p.y - a.y) * ey) / len2, 0.f), 1.f) : 0.f;
    return hypotf(p.x - (a.x + u * ex), p.y - (a.y + u * ey));
}
static bool star_ok(const sky_world_t *w, pt_t p) {
    if (!in_box(p.x, p.y, SKY_MARGIN)) return false;
    for (int i = 0; i < w->count_stars; i++) if (hypotf(w->star[i].x - p.x, w->star[i].y - p.y) < MIN_STAR_GAP) return false;
    for (int i = 0; i < w->count_tools; i++) {
        const sky_tool_t *t = &w->tool[i];
        bool line = t->kind == TOOL_STRING || t->kind == TOOL_SPLIT;
        float near = line ? seg_gap(p, (pt_t){t->x - t->dx, t->y - t->dy}, (pt_t){t->x + t->dx, t->y + t->dy}) : hypotf(t->x - p.x, t->y - p.y);
        if (near < (line ? STAR_TOOL_GAP : 62.f)) return false;
    }
    return hypotf(w->source.x - p.x, w->source.y - p.y) > 72.f;
}
static void sky_star_place(sky_star_t *s, float x, float y) { s->x = s->ax = x; s->y = s->ay = y; }
static bool put_star(sky_world_t *w, const sky_path_t *path, float arc) {
    for (int tries = 0; tries < 1; tries++) {
        pt_t where, dir;
        if (!locate(path, arc, &where, &dir)) return false;
        if (!star_ok(w, where)) continue;
        sky_star_t *s = &w->star[w->count_stars++];
        memset(s, 0, sizeof *s);
        sky_star_place(s, where.x, where.y);
        s->color = (uint8_t)color_at(path, arc);
        return true;
    }
    return false;
}

// How far along the path the light comes nearest to p, px.
static float arc_near(const sky_path_t *p, pt_t where) {
    int best = 0;
    for (int i = 1; i < p->n; i++) if (gap(at(p, i), where) < gap(at(p, best), where)) best = i;
    return p->arc[best];
}
static float relay_arc(const sky_world_t *w, const sky_path_t *p) {
    float arc = 0;
    for (int i = 0; i < w->count_stars; i++) if (w->star[i].relay) arc = fmaxf(arc, arc_near(p, (pt_t){w->star[i].ax, w->star[i].ay}));
    return arc;
}
// Stars on the finished path: its places every 18 px, tried in a random order, the first that fit are taken. Past a
// relay comes first, so that what it sends lights something.
enum { SPOT_STEP = 18, MAX_SPOTS = 64 };
static int shuffled_spots(sky_world_t *w, const sky_path_t *p, int last, int order[MAX_SPOTS]) {
    int spots = 0;
    for (int a = 70; a < p->arc[p->n - 1] && a < 560 && spots < MAX_SPOTS; a += SPOT_STEP) order[spots++] = a;
    for (int i = spots - 1; i > 0; i--) { int j = (int)(sky_rand(w) % (unsigned)(i + 1)), t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int i = 0; last && i < spots; i++) if (order[i] > last + 60) { int t = order[0]; order[0] = order[i]; order[i] = t; break; }
    return spots;
}
static bool scatter_stars(sky_world_t *w, const plan_t *pl, const sky_path_t path[2], int ways) {
    int target = pl->stars + w->count_stars, last = (int)relay_arc(w, &path[0]);
    if (target > SKY_STARS) target = SKY_STARS;
    for (int b = 0; b < ways; b++) {
        int order[MAX_SPOTS], spots = shuffled_spots(w, &path[b], last, order);
        int want = ways == 2 ? (pl->stars + 1 - b) / 2 : pl->stars, taken = 0;
        for (int i = 0; i < spots && taken < want && w->count_stars < target; i++) taken += put_star(w, &path[b], (float)order[i]);
    }
    return w->count_stars >= (target < 2 ? target : target - 1);
}
// Something lies past every relay on the light's way.
static bool fed_past_relays(const sky_world_t *w) {
    sky_path_t path;
    trace_all(w, 0, &path);
    float last = relay_arc(w, &path);
    if (!last) return true;
    for (int i = 0; i < w->count_stars; i++)
        if (!w->star[i].relay && arc_near(&path, (pt_t){w->star[i].ax, w->star[i].ay}) > last + 30.f) return true;
    return false;
}
static bool place_stars(sky_world_t *w, const plan_t *pl, bool finger_only) {
    sky_path_t path[2];
    int ways = branches(w);
    for (int b = 0; b < ways; b++) trace_all(w, b, &path[b]);
    if (finger_only) {  // the first sky: the straight stream misses the star, a finger has to bring it in
        pt_t where, dir;
        if (!locate(&path[0], 230.f, &where, &dir)) return false;
        float side = chance(w, .5f) ? 1.f : -1.f;
        for (int k = 0; k < 2 && !w->count_stars; k++, side = -side) {
            pt_t star = {where.x - dir.y * side * 108.f, where.y + dir.x * side * 108.f};
            if (!star_ok(w, star)) continue;
            sky_star_place(&w->star[0], star.x, star.y);
            w->count_stars = 1;
        }
        return w->count_stars == 1;
    }
    return scatter_stars(w, pl, path, ways);
}

// ---------------------------------------------------------------- hazards and the scramble
static float path_gap(const sky_path_t *p, pt_t where) {
    float best = 1e9f;
    for (int i = 0; i < p->n; i++) best = fminf(best, gap(at(p, i), where));
    return best;
}
static void add_hazards(sky_world_t *w, const plan_t *pl) {
    sky_path_t path[2];
    int ways = branches(w);
    for (int b = 0; b < ways; b++) trace_all(w, b, &path[b]);
    for (int h = 0; h < pl->hungry && w->count_tools < SKY_TOOLS; h++) {
        for (int tries = 0; tries < 24; tries++) {
            pt_t c = {sky_range(w, 80.f, 400.f), sky_range(w, 100.f, 420.f)};
            bool clear = !crowded(w, c.x, c.y);
            for (int b = 0; b < ways; b++) clear = clear && path_gap(&path[b], c) > 118.f;
            for (int i = 0; i < w->count_stars; i++) clear = clear && hypotf(w->star[i].x - c.x, w->star[i].y - c.y) > 110.f;
            if (!clear) continue;
            sky_tool_t *t = &w->tool[w->count_tools++];
            memset(t, 0, sizeof *t);
            t->kind = TOOL_HUNGRY; t->x = c.x; t->y = c.y; t->r = 104.f;
            break;
        }
    }
}
static void add_drift(sky_world_t *w, int count) {
    for (int i = 0; i < w->count_stars && count > 0; i++) {
        if (w->star[i].orbit > 0 || !chance(w, .6f)) continue;
        sky_star_t *s = &w->star[i];
        s->orbit = sky_range(w, 12.f, 22.f);
        s->spin = sky_range(w, .45f, .8f) * (chance(w, .5f) ? 1.f : -1.f);
        s->phase = sky_range(w, 0.f, 2 * PI_F);
        count--;
    }
}
// How many stars the light traced through the sky as it stands passes through.
static int lit_by_trace(const sky_world_t *w) {
    bool lit[SKY_STARS] = {false};
    sky_path_t path;
    for (int b = 0; b < branches(w); b++) {
        trace_all(w, b, &path);
        for (int i = 0; i < w->count_stars; i++)
            if (path_gap(&path, (pt_t){w->star[i].ax, w->star[i].ay}) < SKY_ZONE_R * .85f) lit[i] = true;
    }
    int n = 0;
    for (int i = 0; i < w->count_stars; i++) n += lit[i];
    return n;
}
// Some star is well away from where the light goes (so that the stream's spread cannot light it either).
static bool misses_a_star(const sky_world_t *w) {
    sky_path_t path[2];
    int ways = branches(w);
    for (int b = 0; b < ways; b++) trace_all(w, b, &path[b]);
    for (int i = 0; i < w->count_stars; i++) {
        float best = 1e9f;
        for (int b = 0; b < ways; b++) best = fminf(best, path_gap(&path[b], (pt_t){w->star[i].ax, w->star[i].ay}));
        if (best > SKY_ZONE_R + 36.f) return true;
    }
    return false;
}
static void scramble_tool(sky_world_t *w, sky_tool_t *t, float amount) {
    float a = sky_range(w, 0.f, 2 * PI_F), m = sky_range(w, .8f, 1.2f) * amount;
    float x = t->sx + m * cosf(a), y = t->sy + m * sinf(a);
    t->x = fminf(fmaxf(x, 40.f), SKY_W - 40.f); t->y = fminf(fmaxf(y, 56.f), SKY_W - 40.f);
    if (t->kind != TOOL_STRING && t->kind != TOOL_SPLIT) return;
    float turn = (chance(w, .5f) ? 1.f : -1.f) * sky_range(w, .45f, 1.1f), c = cosf(turn), s = sinf(turn);
    t->dx = t->sdx * c - t->sdy * s; t->dy = t->sdx * s + t->sdy * c;
}
// A relay turned to another of its ways; an aimed source turned by 35..80 degrees.
static void scramble_relays(sky_world_t *w) {
    for (int i = 0; i < w->count_stars; i++)
        if (w->star[i].relay) w->star[i].dir = (uint8_t)((w->star[i].sdir + sky_pick(w, 1, SKY_DIRS - 1)) % SKY_DIRS);
    sky_source_t *s = &w->source;
    if (!s->aim) return;
    float turn = (chance(w, .5f) ? 1.f : -1.f) * sky_range(w, .6f, 1.4f), c = cosf(turn), sn = sinf(turn);
    s->dx = s->sdx * c - s->sdy * sn; s->dy = s->sdx * sn + s->sdy * c;
}
void sky_scramble_back(sky_world_t *w) {
    for (int i = 0; i < w->count_tools; i++) {
        sky_tool_t *t = &w->tool[i];
        if (!t->movable) continue;
        t->x = t->sx; t->y = t->sy; t->dx = t->sdx; t->dy = t->sdy;
    }
    for (int i = 0; i < w->count_stars; i++) w->star[i].dir = w->star[i].sdir;
    w->source.dx = w->source.sdx; w->source.dy = w->source.sdy;
}
// ---------------------------------------------------------------- one try
static void tune_stars(sky_world_t *w, int root) {
    static const uint8_t ladder[SKY_STARS] = {0, 2, 4, 5, 7, 9};
    w->root = (uint8_t)root;
    for (int i = 0; i < w->count_stars; i++) {
        int degree = root + ladder[i];
        while (degree > 9) degree -= 5;
        w->star[i].degree = (uint8_t)degree;
        w->star[i].hz = sky_degree_hz(degree);
    }
}
static bool pour_layout(sky_world_t *w, const plan_t *pl) {
    float angle = sky_range(w, 0.f, 2 * PI_F), push = sky_range(w, .38f, .6f);
    w->gravity[0] = cosf(angle) * push; w->gravity[1] = sinf(angle) * push;
    if (!place_stars(w, pl, false)) return false;
    float g[2] = {w->gravity[0], w->gravity[1]};
    w->gravity[0] = w->gravity[1] = 0;
    bool solved_flat = lit_by_trace(w) >= w->count_stars;   // the stream would light them without any tilt
    w->gravity[0] = g[0]; w->gravity[1] = g[1];
    return !solved_flat;
}
// ---------------------------------------------------------------- the composer: one slice of work at a time
// A slice is one trace-heavy step (a tool tried, the stars, one round of the scramble), so that a sky can grow
// between frames without ever holding one up.
enum { ST_START, ST_POUR, ST_TOOLS, ST_STARS, ST_VERIFY, ST_SCRAMBLE };
enum { GO_ON, GO_FAIL, GO_DONE };
#define TOOL_TRIES 20
#define SCRAMBLE_ROUNDS 5
#define VERIFY_STEPS_PER_SLICE 6
#define VERIFY_MAX_STEPS 270    // 9 s of flow
#define VERIFY_HELD 30          // ...and every star full for a second of it

static int finish(sky_composer_t *c) { tune_stars(c->w, c->r.root); return GO_DONE; }
static int slice_start(sky_composer_t *c) {
    sky_world_t *w = c->w;
    memset(w, 0, sizeof *w);
    w->rng = c->simple ? c->seed * 40503u + c->attempt * 2654435761u + 7u : c->seed * 2654435761u + c->attempt * 40503u + 1u;
    w->feed_scale = 1.f;
    sky_choose_plan(w, &c->r, &c->pl);
    sky_place_source(w, &c->r, c->pl.pour);
    w->source.aim = c->pl.aim;
    c->stage = c->pl.pour ? ST_POUR : c->pl.seq_n ? ST_TOOLS : ST_STARS;
    c->j = c->tries = 0;
    c->last = 0;
    return GO_ON;
}
static int tools_end(sky_composer_t *c, int placed) {
    if (placed < 0) return GO_FAIL;
    c->pl.seq_n = (uint8_t)placed;
    c->stage = ST_STARS;
    return GO_ON;
}
// One try at the next tool along the light; the plan is cut to what fits (-1: not even the first does).
static int slice_tools(sky_composer_t *c) {
    sky_world_t *w = c->w;
    int j = c->j, placed = j ? j : -1;
    sky_path_t path;
    trace_all(w, 0, &path);
    float lo = bends(c->pl.seq[j]) ? 92.f : 82.f, hi = fminf(lo + 40.f, path_room(&path) - c->last - 50.f);
    if (hi < 50.f) return tools_end(c, placed);   // the light has no more room for another tool
    float want = c->last + sky_range(w, fminf(lo, hi - 10.f), hi);
    if (try_tool(w, &c->pl, j, want, &path)) {
        c->last = want; c->tries = 0;
        return ++c->j >= c->pl.seq_n ? tools_end(c, c->pl.seq_n) : GO_ON;
    }
    return ++c->tries >= TOOL_TRIES ? tools_end(c, placed) : GO_ON;
}
// The layout, with its tools where the solution has them, is made to flow with a few lights; every star must fill
// and stay full.
static int verify_begin(sky_composer_t *c) {
    c->probe = *c->w;
    c->probe.feed_scale = (float)SKY_PARTICLES / SKY_PROBE_N;
    for (int i = 0; i < c->probe.count_stars; i++) c->probe.star[i].fill = 0;
    sky_stream_launch(&c->probe, c->light, SKY_PROBE_N, NULL);
    c->steps = 0; c->tries = 0;
    c->stage = ST_VERIFY;
    return GO_ON;
}
static int after_verify(sky_composer_t *c) {
    if (c->pl.pour) return finish(c);
    c->stage = ST_SCRAMBLE;
    c->tries = 0;
    return GO_ON;
}
static int slice_verify(sky_composer_t *c) {
    sky_force_t force = {.gx = c->w->gravity[0], .gy = c->w->gravity[1]};
    for (int n = 0; n < VERIFY_STEPS_PER_SLICE; n++, c->steps++) {
        sky_stream_step(&c->probe, c->light, SKY_PROBE_N, 1.f / 30.f, &force, NULL);
        c->tries = sky_solved(&c->probe) ? c->tries + 1 : 0;
        if (c->tries >= VERIFY_HELD) return after_verify(c);
    }
    return c->steps >= VERIFY_MAX_STEPS ? GO_FAIL : GO_ON;
}
static int slice_stars(sky_composer_t *c) {
    sky_world_t *w = c->w;
    if (!place_stars(w, &c->pl, c->r.first) || !fed_past_relays(w)) return GO_FAIL;
    add_hazards(w, &c->pl);
    add_drift(w, c->pl.drift);
    if (c->r.first) return lit_by_trace(w) >= w->count_stars ? GO_FAIL : finish(c);
    return verify_begin(c);
}
// One round: every movable tool thrown from its place; done when some star is missed.
static int slice_scramble(sky_composer_t *c) {
    sky_world_t *w = c->w;
    int movable = w->source.aim;
    for (int i = 0; i < w->count_tools; i++) movable += w->tool[i].movable;
    for (int i = 0; i < w->count_stars; i++) movable += w->star[i].relay;
    if (!movable) return finish(c);
    float amount = 70.f + c->r.skill * .25f + (float)c->tries * 22.f;
    for (int i = 0; i < w->count_tools; i++) if (w->tool[i].movable) scramble_tool(w, &w->tool[i], amount);
    scramble_relays(w);
    if (misses_a_star(w)) return finish(c);
    return ++c->tries >= SCRAMBLE_ROUNDS ? GO_FAIL : GO_ON;
}
static int slice(sky_composer_t *c) {
    switch (c->stage) {
    case ST_START: return slice_start(c);
    case ST_POUR: return pour_layout(c->w, &c->pl) ? verify_begin(c) : GO_FAIL;
    case ST_VERIFY: return slice_verify(c);
    case ST_TOOLS: return slice_tools(c);
    case ST_STARS: return slice_stars(c);
    default: return slice_scramble(c);
    }
}

void sky_compose_begin(sky_composer_t *c, sky_world_t *w, uint32_t seed, const sky_recipe_t *r) {
    memset(c, 0, sizeof *c);
    c->w = w; c->seed = seed; c->r = *r;
}
// A layout that fails is dropped and the seed advanced; after ATTEMPTS, the simplest sky: one star beside the stream.
bool sky_compose_step(sky_composer_t *c) {
    int result = slice(c);
    if (result == GO_ON) return false;
    if (result == GO_DONE) return true;
    c->stage = ST_START;
    if (!c->simple && c->attempt + 1 == ATTEMPTS / 2) c->r.skill /= 2;   // a sky that will not fit asks for less
    if (++c->attempt < (c->simple ? ATTEMPTS * 4 : ATTEMPTS)) return false;
    if (c->simple) return true;
    c->simple = true;
    c->attempt = 0;
    c->r.first = true; c->r.aurora = false; c->r.elements = EL_POUR;
    c->r.from[0] = SKY_CX - 150.f; c->r.from[1] = SKY_CY;
    return false;
}
void sky_compose(sky_world_t *w, uint32_t seed, const sky_recipe_t *r) {
    sky_composer_t c;
    sky_compose_begin(&c, w, seed, r);
    while (!sky_compose_step(&c)) {}
}
