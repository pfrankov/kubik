#include "tess_sky_internal.h"

#include <math.h>
#include <string.h>

// What a sky will hold (a plan for tess_sky_compose.c), and where its light comes from. The element the player has just
// met is always in it; the rest come by chance and grow with how well they are doing.
#define PI_F 3.14159265f
typedef sky_plan_t plan_t;

int sky_pick(sky_world_t *w, int lo, int hi) { return lo + (int)(sky_rand(w) % (unsigned)(hi - lo + 1)); }
static bool chance(sky_world_t *w, float p) { return sky_randf(w) < p; }
static void add_seq(plan_t *pl, uint8_t kind) { if (pl->seq_n < 4) pl->seq[pl->seq_n++] = kind; }

static void plan_stars(sky_world_t *w, const sky_recipe_t *r, plan_t *pl) {
    int e = r->elements, skill = r->skill;
    int most = e <= 2 ? 1 : e <= 3 ? 2 : e <= 5 ? 3 : skill > 180 ? 5 : 4, least = e <= 2 ? 1 : 2;
    pl->stars = (uint8_t)(r->first ? 1 : least + (skill * (most - least + 1)) / 256 + (chance(w, .4f) ? 1 : 0));
    if (pl->stars > most) pl->stars = (uint8_t)most;
}
static void plan_strings(sky_world_t *w, const sky_recipe_t *r, plan_t *pl) {
    bool harp = w->kind == SKY_HARP;
    if (r->elements <= EL_STRING) return;
    int strings = harp ? sky_pick(w, 2, 3) : 1 + (chance(w, .35f + r->skill / 600.f) ? 1 : 0) + (r->skill > 170 ? 1 : 0);
    if (r->newest == EL_STRING) strings = harp ? 2 : 1 + (chance(w, .5f) ? 1 : 0);
    for (int i = 0; i < strings; i++) add_seq(pl, TOOL_STRING);
}
static bool has_well(const plan_t *pl) {   // (a splitter and a well together scatter the stream beyond what a trace can promise)
    for (int i = 0; i < pl->seq_n; i++) if (pl->seq[i] == TOOL_WELL) return true;
    return false;
}
static bool wants(sky_world_t *w, const sky_recipe_t *r, int el, float p) { return r->elements > el && (r->newest == el || chance(w, p)); }
static void plan_hazards(sky_world_t *w, const sky_recipe_t *r, plan_t *pl) {   // (and a relay: it is no tool)
    if (!pl->split && wants(w, r, EL_RELAY, .45f) && pl->stars < SKY_STARS) { add_seq(pl, SKY_SEQ_RELAY); pl->relay = true; }
    if (wants(w, r, EL_HUNGRY, .5f)) pl->hungry = (uint8_t)(1 + (r->skill > 190 ? 1 : 0));
    if (wants(w, r, EL_DRIFT, .4f)) pl->drift = (uint8_t)sky_pick(w, 1, pl->stars);
}
static void plan_tools(sky_world_t *w, const sky_recipe_t *r, plan_t *pl) {
    plan_strings(w, r, pl);
    if (w->kind == SKY_HARP) return;
    pl->aim = wants(w, r, EL_AIM, .35f);
    if (r->elements > EL_WELL && (r->newest == EL_WELL || (r->newest != EL_SPLIT && chance(w, .5f)))) add_seq(pl, TOOL_WELL);
    if (wants(w, r, EL_PRISM, .42f)) add_seq(pl, TOOL_PRISM);
    if (wants(w, r, EL_SPLIT, .3f) && pl->stars >= 2 && !has_well(pl) && r->newest != EL_RELAY) { add_seq(pl, TOOL_SPLIT); pl->split = true; }
    plan_hazards(w, r, pl);
}
static void newest_first(plan_t *pl, int newest) {   // the element the player is just meeting is never the one that is cut
    static const uint8_t kind_of[EL_COUNT] = {0, 0, TOOL_STRING, SKY_SEQ_RELAY, TOOL_WELL, 0, TOOL_PRISM, TOOL_SPLIT, 0, 0};
    for (int i = 1; i < pl->seq_n && kind_of[newest]; i++)
        if (pl->seq[i] == kind_of[newest]) { pl->seq[i] = pl->seq[0]; pl->seq[0] = kind_of[newest]; break; }
}
void sky_choose_plan(sky_world_t *w, const sky_recipe_t *r, sky_plan_t *pl) {
    memset(pl, 0, sizeof *pl);
    int e = r->elements;
    w->kind = SKY_PUZZLE;
    if (r->aurora) w->kind = SKY_AURORA;
    else if (!r->first && e > EL_POUR + 0 && r->newest == EL_POUR) w->kind = SKY_POUR;
    else if (!r->first && e > EL_POUR && chance(w, .14f)) w->kind = SKY_POUR;
    else if (!r->first && e > EL_STRING && r->newest != EL_STRING && chance(w, .16f)) w->kind = SKY_HARP;
    pl->pour = w->kind == SKY_POUR;
    plan_stars(w, r, pl);
    if (pl->pour) return;
    plan_tools(w, r, pl);
    newest_first(pl, r->newest);
}

// ---------------------------------------------------------------- the source
void sky_place_source(sky_world_t *w, const sky_recipe_t *r, bool pour) {
    sky_source_t *s = &w->source;
    float a = sky_range(w, 0.f, 2 * PI_F), reach = sky_range(w, 140.f, 168.f);
    s->x = SKY_CX + reach * cosf(a); s->y = SKY_CY + reach * sinf(a);
    if (r->first) { s->x = r->from[0]; s->y = r->from[1]; }
    float tx = SKY_CX + sky_range(w, -90.f, 90.f), ty = SKY_CY + sky_range(w, -90.f, 90.f);
    float dx = tx - s->x, dy = ty - s->y, len = hypotf(dx, dy);
    if (len < 1.f) { dx = 1; dy = 0; len = 1; }
    s->dx = dx / len; s->dy = dy / len;
    s->speed = pour ? 110.f : sky_range(w, 112.f, 136.f);
    s->sdx = s->dx; s->sdy = s->dy;
}

