// Skies: every composed sky is solved by its own solution and not by its scramble.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../main/tess_sky_internal.h"

#define SECONDS 40
static int fails, bad[EL_COUNT + 1][4], made[EL_COUNT + 1][4], feat_bad[256], feat_all[256];
static int features(const sky_world_t *w) {   // hungry, drift, prism, split, well, string, relay, aim
    int m = w->source.aim ? 128 : 0;
    for (int i = 0; i < w->count_tools; i++) m |= 1 << (w->tool[i].kind == TOOL_HUNGRY ? 0 : w->tool[i].kind == TOOL_PRISM ? 2 : w->tool[i].kind == TOOL_SPLIT ? 3 : w->tool[i].kind == TOOL_WELL ? 4 : 5);
    for (int i = 0; i < w->count_stars; i++) m |= (w->star[i].orbit > 0 ? 2 : 0) | (w->star[i].relay ? 64 : 0);
    return m;
}
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 6) { printf("FAIL " __VA_ARGS__); printf("\n"); } } } while (0)

static int run(sky_world_t *w, bool tilt, bool finger, float *best_min) {
    static sky_particle_t p[SKY_PARTICLES];
    sky_force_t f = {.gx = tilt ? w->gravity[0] : 0, .gy = tilt ? w->gravity[1] : 0};
    if (finger) { f.finger = true; f.fx = w->star[0].x; f.fy = w->star[0].y; }
    for (int i = 0; i < w->count_stars; i++) w->star[i].fill = 0;
    sky_stream_launch(w, p, SKY_PARTICLES, NULL);
    float held = 0, most = 0;
    for (int fr = 0; fr < SECONDS * 30; fr++) {
        sky_stream_step(w, p, SKY_PARTICLES, 1.f / 30, &f, NULL);
        float lo = 1;
        for (int i = 0; i < w->count_stars; i++) lo = w->star[i].fill < lo ? w->star[i].fill : lo;
        if (lo > most) most = lo;
        held = lo >= SKY_FULL ? held + 1.f / 30 : 0;
        if (held >= SKY_HOLD_S) { *best_min = most; return fr; }
    }
    *best_min = most;
    return -1;
}

static void detail(const sky_world_t *w, int seed, int skill) {
    printf("-- seed %d skill %d kind %d src %.0f,%.0f dir %.2f,%.2f sp %.0f\n", seed, skill, w->kind, w->source.x, w->source.y, w->source.dx, w->source.dy, w->source.speed);
    for (int i = 0; i < w->count_tools; i++) printf(" tool k%d %.0f,%.0f d %.0f,%.0f c%d\n", w->tool[i].kind, w->tool[i].x, w->tool[i].y, w->tool[i].dx, w->tool[i].dy, w->tool[i].color);
    for (int i = 0; i < w->count_stars; i++) printf(" star %.0f,%.0f c%d fill %.2f relay %d dir %d\n", w->star[i].x, w->star[i].y, w->star[i].color, w->star[i].fill, w->star[i].relay, w->star[i].dir);
    sky_path_t path; sky_force_t f = {0}; sky_trace(w, 0, &f, 230, 3, &path);
    for (int i = 0; i < path.n; i += 3) printf("%d:%d,%d,c%d ", path.arc[i], path.x[i] / 8, path.y[i] / 8, path.color[i]);
    printf("\n");
}
static int by_kind[4], total, solved, scrambled_solved, stars, tools;
// One composed sky: deterministic, on the scale, solvable by its own solution, and its scramble is not the solution.
static void check_scale(const sky_world_t *w) {
    for (int i = 0; i < w->count_stars; i++) {
        bool ok = false;
        for (int d = 0; d < 10; d++) if (w->star[i].hz == sky_degree_hz(d)) ok = true;
        CHECK(ok, "off scale %f", w->star[i].hz);
    }
}
static void check_one(int skill, int el, int s) {
    sky_recipe_t r = {.elements = (uint8_t)el, .newest = (uint8_t)(el - 1), .skill = (uint8_t)skill, .root = (uint8_t)(s % 5),
        .first = el == 1, .from = {120, 300}};
    sky_world_t w, w2;
    sky_compose(&w, (uint32_t)s * 7919u + el, &r);
    sky_compose(&w2, (uint32_t)s * 7919u + el, &r);
    CHECK(!memcmp(&w, &w2, sizeof w), "not deterministic seed %d", s);
    feat_all[features(&w)]++;
    total++; made[el][w.kind]++; stars += w.count_stars; tools += w.count_tools; by_kind[w.kind]++;
    CHECK(w.count_stars >= 1, "no stars seed %d el %d", s, el);
    check_scale(&w);
    sky_world_t sol = w; float m1, m2;
    sky_scramble_back(&sol);
    bool pour = w.kind == SKY_POUR;
    bool relays = false;
    for (int i = 0; i < w.count_stars; i++) relays |= w.star[i].relay;
    int t = run(&sol, pour, !pour && (el == 1 || (w.count_tools == 0 && !relays && !w.source.aim)), &m1);
    if (t >= 0) solved++; else { bad[el][w.kind]++; feat_bad[features(&w)]++; if (getenv("DETAIL") && bad[el][w.kind] <= 2 && el == atoi(getenv("DETAIL"))) detail(&sol, s, skill); printf("unsolved: skill %d el %d seed %d kind %d stars %d tools %d (best %.2f)\n", skill, el, s, w.kind, w.count_stars, w.count_tools, m1); }
    if (!pour && el > 1) { sky_world_t sc = w; if (run(&sc, false, false, &m2) >= 0) scrambled_solved++; }
}
int main(int argc, char **argv) {
    int seeds = argc > 1 ? atoi(argv[1]) : 120;
    for (int skill = 0; skill <= 255; skill += 85) for (int el = 1; el <= EL_COUNT; el++) for (int s = 1; s <= seeds; s++) check_one(skill, el, s);
    printf("skies %d solved %d, scrambled already solved %d, avg stars %.2f tools %.2f kinds P%d O%d H%d A%d\n", total, solved, scrambled_solved,
           (double)stars / total, (double)tools / total, by_kind[0], by_kind[1], by_kind[2], by_kind[3]);
    for (int e = 1; e <= EL_COUNT; e++) printf("el %d: bad P%d/%d O%d/%d H%d/%d\n", e, bad[e][0], made[e][0], bad[e][1], made[e][1], bad[e][2], made[e][2]);
    if (getenv("FEATURES")) for (int m = 0; m < 256; m++) if (feat_all[m]) printf("features %02x (hungry1 drift2 prism4 split8 well16 string32 relay64 aim128): bad %d / %d\n", m, feat_bad[m], feat_all[m]);
    // The composer proves its layouts by tracing; the rare edge it cannot (a splitter on a prism) must stay rare.
    CHECK(total - solved <= total / 500, "%d of %d skies cannot be solved", total - solved, total);
    CHECK(scrambled_solved <= total / 50, "%d of %d scrambles are already solved", scrambled_solved, total);
    return fails != 0;
}
