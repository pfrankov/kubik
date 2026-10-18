#include "tess_sky_internal.h"

#include <math.h>
#include <string.h>

// A small vocabulary, with new geometry every time: a finger, tilt, one or two black holes, then drifting stars.
// Ten saved progression steps still fit old progress records; none of them unlocks a removed tool.
#define PI_F 3.14159265f
int sky_pick(sky_world_t *w, int lo, int hi) { return lo + (int)(sky_rand(w) % (unsigned)(hi - lo + 1)); }
static int star_budget(const sky_recipe_t *r) {
    if (r->elements <= 2) return 1;
    if (r->elements < 5) return 2;
    return r->skill < 150 ? 3 : 5;
}

void sky_choose_plan(sky_world_t *w, const sky_recipe_t *r, sky_plan_t *pl) {
    memset(pl, 0, sizeof *pl);
    w->kind = r->aurora ? SKY_AURORA : r->elements == 2 ? SKY_POUR : SKY_PUZZLE;
    pl->pour = w->kind == SKY_POUR;
    if (r->aurora) { pl->holes = SKY_HOLES; return; }
    if (r->first) { pl->stars = 1; return; }
    int most = star_budget(r);
    pl->stars = (uint8_t)sky_pick(w, most > 2 ? most - 1 : most, most);
    pl->holes = (uint8_t)(r->elements < 3 ? 0 : r->elements < 6 ? 1 : sky_pick(w, 1, SKY_HOLES));
    if (r->elements >= 7 && sky_randf(w) < .55f) pl->drift = (uint8_t)sky_pick(w, 1, pl->stars);
}
void sky_place_source(sky_world_t *w, const sky_recipe_t *r, bool pour) {
    sky_source_t *s = &w->source;
    float angle = sky_range(w, 0.f, 2.f * PI_F), reach = sky_range(w, 140.f, 170.f);
    s->x = SKY_CX + reach * cosf(angle); s->y = SKY_CY + reach * sinf(angle);
    if (r->first) { s->x = r->from[0]; s->y = r->from[1]; }
    float tx = SKY_CX + sky_range(w, -55.f, 55.f), ty = SKY_CY + sky_range(w, -55.f, 55.f);
    float dx = tx - s->x, dy = ty - s->y, len = hypotf(dx, dy);
    if (len < 1.f) { dx = 1; dy = 0; len = 1; }
    s->dx = dx / len; s->dy = dy / len;
    s->speed = pour ? 112.f : sky_range(w, 115.f, 135.f);
}
