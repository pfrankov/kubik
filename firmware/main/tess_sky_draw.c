#include "tess_sky_play.h"
#include "palette_blend.h"

// The picture of a sky: stars with beads that light as they fill (and flash as they become full), relays with an arrow of
// dots for where they send the light, strings, wells, prisms, the source with a nozzle of dots for its way; and, on
// resolving, a line of dots that joins the stars as the melody reaches each. The stream itself is her own dots, drawn by
// the cloud (tess_draw.c) at the places the sky gives them. Movable things shimmer a little; the one that is hinted
// trembles towards where it is wanted (a relay or the source shows a faint ghost of its right way). Before a sky, the
// invitation peels from her corner as a short thread of gold.
#define GLOW_ALPHA 255
#define MAX_LINE_DOTS 36
#define LINE_GAP 17.f

static const uint32_t k_colour[COL_COUNT] = {0x2BE2D2, 0xFFCB5C, 0xFF8AC4};
#define WELL_COLOUR 0x9A7BFF
#define SPLIT_COLOUR 0xBFE4FF
#define DARK_COLOUR 0x6B2BD9

// A star: a glow, and six beads around its zone that light one after the other as it fills. (Beads are dots, which
// cost a fraction of what a ring and an arc would; once resolving, the constellation's dots take their room.)
#define BEADS 6
static const float k_bead[BEADS][2] = {{0, -1}, {.866f, -.5f}, {.866f, .5f}, {0, 1}, {-.866f, .5f}, {-.866f, -.5f}};
static void draw_star(scene_t *s, const sky_star_t *st, float appear, float t, bool beads) {
    uint32_t c = k_colour[st->color];
    float pulse = .5f + .5f * tess_sin(t * 2.2f + st->x * .05f), fill = st->fill, full = fill >= SKY_FULL ? 1.f : 0.f, flash = st->glow;
    sc_glow_dot(s, st->x, st->y, 2.6f + 4.2f * fill + 1.4f * full * pulse + 4.f * flash, palette_mix(rgb_scale(c, .55f + .45f * fill), 0xFFFFFF, .5f * flash),
                (uint8_t)(GLOW_ALPHA * appear * clampf(.35f + .65f * fill + flash, 0.f, 1.f)));
    float ring = SKY_ZONE_R + 9.f * flash;   // the beads burst out a little as it becomes full
    for (int b = 0; beads && b < BEADS; b++) {
        float lit = clampf(fill * (float)BEADS - (float)b, 0.f, 1.f);
        float a = appear * (.2f + .12f * pulse + .2f * full + .6f * lit);
        sc_glow_dot(s, st->x + k_bead[b][0] * ring, st->y + k_bead[b][1] * ring, 1.5f + 1.4f * lit, rgb_scale(c, .6f + .4f * lit), (uint8_t)(255.f * clampf(a, 0.f, 1.f)));
    }
}
// A chevron of five dots pointing along (dx, dy), its tip `at` px out from (x, y).
static void arrow(scene_t *s, float x, float y, float dx, float dy, float at, uint32_t c, float a) {
    uint8_t alpha = (uint8_t)(255.f * clampf(a, 0.f, 1.f));
    sc_glow_dot(s, x + dx * at, y + dy * at, 2.6f, c, alpha);
    for (int i = 1; i <= 2; i++)
        for (int w = -1; w <= 1; w += 2) {
            float back = at - 8.f * (float)i, side = 8.f * (float)(i * w);
            sc_glow_dot(s, x + dx * back - dy * side, y + dy * back + dx * side, 2.4f - .4f * (float)i, c, alpha);
        }
}
// A relay's arrow: dim until it is full and sends the light, then bright, flowing outwards. Hinted: a ghost of its right way.
static void draw_relay(scene_t *s, const sky_star_t *st, float appear, float t, bool hinted) {
    float dx, dy, on = st->fill >= SKY_FULL ? 1.f : .35f, flow = 3.f * (t * 1.5f - floorf(t * 1.5f));
    sky_dir_vector(st->dir, &dx, &dy);
    arrow(s, st->x, st->y, dx, dy, SKY_ZONE_R + 28.f + flow * on, palette_mix(k_colour[st->color], 0xFFFFFF, .55f), appear * on);
    if (!hinted) return;
    sky_dir_vector(st->sdir, &dx, &dy);
    arrow(s, st->x, st->y, dx, dy, SKY_ZONE_R + 28.f, 0xFFFFFF, appear * (.25f + .2f * tess_sin(t * 6.f)));
}
// The source: a glow and a nozzle of dots for where it sends the light; an aimed one is larger and shimmers.
static void draw_source(scene_t *s, const sky_source_t *src, float appear, float t, bool hinted) {
    float breathe = tess_sin(t * 2.f), a = appear * (src->aim ? .85f + .15f * tess_sin(t * 3.f) : .5f);
    sc_glow_dot(s, src->x, src->y, (src->aim ? 5.5f : 4.f) + 1.5f * breathe, k_colour[COL_TEAL], (uint8_t)(200.f * a));
    if (src->aim) arrow(s, src->x, src->y, src->dx, src->dy, 30.f, 0xBFFFF6, a);
    if (hinted) arrow(s, src->x, src->y, src->sdx, src->sdy, 30.f, 0xFFFFFF, appear * (.25f + .2f * tess_sin(t * 6.f)));
}

static void draw_string(scene_t *s, const sky_tool_t *t, float x, float y, float a, uint32_t c) {
    sc_capsule(s, x - t->dx, y - t->dy, x + t->dx, y + t->dy, 2.6f + 1.6f * t->shown, c, a * (.6f + .4f * t->shown));
}

static void draw_tool(scene_t *s, const sky_tool_t *t, float appear, float time, bool hinted) {
    float shimmer = .85f + .15f * tess_sin(time * 3.f + t->x * .07f), a = appear * (t->movable ? shimmer : 1.f);
    float x = t->x, y = t->y;
    if (hinted) {   // trembles towards where it is wanted
        float len = hypotf(t->sx - x, t->sy - y) + .01f, shake = 6.f * tess_sin(time * 11.f);
        x += (t->sx - x) / len * shake; y += (t->sy - y) / len * shake;
    }
    switch (t->kind) {
    case TOOL_STRING: draw_string(s, t, x, y, a, 0xE8FFF9); break;
    case TOOL_SPLIT: draw_string(s, t, x, y, a, SPLIT_COLOUR); break;
    case TOOL_WELL:
        sc_ring(s, x, y, 26.f, 2.4f, WELL_COLOUR, a * (.55f + .03f * (float)t->held));
        break;
    case TOOL_PRISM:
        sc_rbox(s, x, y, 12.f, 12.f, 4.f, time * .7f, k_colour[t->color], a * .9f);
        break;
    default:
        sc_ring(s, x, y, 22.f, 4.f, 0x2A0A3C, a * .95f);
    }
}

// When the melody first sings star i, ms from its start (-1: never).
static float star_ms(const sky_melody_t *m, int star) {
    for (int i = 0; i < m->n; i++) if (m->note[i].star == star) return sky_melody_start_ms(m, i);
    return -1.f;
}
// A line of dots from star to star along the melody's tour: each stretch is drawn out while the melody walks from one
// star's note to the next, and arrives with it.
static void draw_constellation(scene_t *s, const tess_sky_t *k) {
    const sky_world_t *w = &k->w;
    const sky_melody_t *m = &k->melody;
    float ms = (k->resolve_t - SKY_MELODY_AT_S) * 1000.f, fade = 1.f - smooth01((k->resolve_t - (k->resolve_len - 1.f)) / .9f);
    int dots = 0;
    for (int i = 0; i + 1 < w->count_stars && m->tour[i + 1] >= 0 && dots < MAX_LINE_DOTS; i++) {
        const sky_star_t *a = &w->star[m->tour[i]], *b = &w->star[m->tour[i + 1]];
        float t0 = star_ms(m, m->tour[i]), t1 = star_ms(m, m->tour[i + 1]);
        float reveal = t0 < 0 || t1 <= t0 ? 0.f : clampf((ms - t0) / (t1 - t0), 0.f, 1.f);
        float dx = b->x - a->x, dy = b->y - a->y, len = hypotf(dx, dy);
        int steps = (int)(len / LINE_GAP);
        for (int d = 1; d < steps && dots < MAX_LINE_DOTS; d++, dots++) {
            float u = (float)d / (float)steps, shown = clampf((reveal - u) * 4.f, 0, 1);
            if (shown > 0) sc_glow_dot(s, a->x + dx * u, a->y + dy * u, 1.7f + .6f * shown, 0xFFD98A, (uint8_t)(150 * shown * fade));
        }
    }
}

// One leading bead and a short fading thread: the same light that becomes the stream.
#define CALL_COLOUR 0xFFD66E
static void draw_call(const face_t *f, scene_t *s) {
    float x, y, shown = sky_summon_shown(f);
    if (!tess_sky_calling(f, &x, &y) || shown <= 0) return;
    float pulse = .5f + .5f * tess_sin(f->t * 2.7f);
    for (int i = 5; i >= 1; i--) {
        float tx, ty;
        sky_summon_point(f, .065f * (float)i, &tx, &ty);
        float tail = 1.f - (float)i / 6.f;
        sc_glow_dot(s, tx, ty, 1.1f + 1.2f * tail, CALL_COLOUR, (uint8_t)(175.f * shown * tail));
    }
    sc_glow_dot(s, x, y, 3.5f + .7f * pulse, CALL_COLOUR, (uint8_t)(255.f * shown));
    sc_glow_dot(s, x, y, 1.4f, 0xFFF4D6, (uint8_t)(245.f * shown));
    float spread = 7.f + 2.f * pulse;
    sc_glow_dot(s, x - spread, y + spread * .6f, 1.2f, CALL_COLOUR, (uint8_t)(125.f * shown));
    sc_glow_dot(s, x + spread * .6f, y - spread, 1.f, CALL_COLOUR, (uint8_t)(95.f * shown));
}

void tess_sky_draw(const face_t *f, scene_t *s) {
    const tess_sky_t *k = f->tess_games.sky;
    if (!k) draw_call(f, s);
    if (!k || f->tess_games.game != TESS_GAME_SKY || k->phase == SKY_UNRAVEL) return;
    const sky_world_t *w = &k->w;
    float appear = smooth01(w->t / 1.2f) * (1.f - smooth01(k->fade / SKY_FADE_S)), time = f->t;
    draw_source(s, &w->source, appear, time, k->hint_tool == SKY_HINT_SOURCE);
    for (int i = 0; i < w->count_tools; i++) draw_tool(s, &w->tool[i], appear, time, i == k->hint_tool);
    for (int i = 0; i < w->count_stars; i++) {
        draw_star(s, &w->star[i], appear, time, k->phase != SKY_RESOLVE);
        if (w->star[i].relay && k->phase != SKY_RESOLVE) draw_relay(s, &w->star[i], appear, time, k->hint_tool == SKY_HINT_RELAY(i));
    }
    if (k->phase == SKY_RESOLVE) draw_constellation(s, k);
}

// The colour the stream's dot carries: 0 her own, else a prism's.
int tess_sky_dot_colour(const face_t *f, int point) {
    const tess_sky_t *k = f->tess_games.sky;
    return k && f->tess_games.game == TESS_GAME_SKY ? k->p[f->tess_scatter_of[point]].color : 0;
}
