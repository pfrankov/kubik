#include "tess_sky_play.h"
#include "palette_blend.h"

// Stars gather the same small lights as the stream into a lasting core. Gravity has no drawing of its own:
// permanent and finger-created black holes are visible only through the motion of Tess's 112 dots.
#define MAX_LINE_DOTS 36
#define LINE_GAP 17.f
#define MAX_STREAM_TRAILS 20

static const uint32_t k_colour[COL_COUNT] = {0x75E9D6, 0xFFD179, 0xEDA7D6};

// Glow changes a dot's halo, not its opaque centre: fade its colour too.
static void light(scene_t *s, float x, float y, float radius, uint32_t c, float shown, float glow) {
    if (shown <= .004f) return;
    shown = clampf(shown, 0.f, 1.f);
    sc_glow_dot(s, x, y, radius, rgb_scale(c, shown), (uint8_t)(255.f * shown * clampf(glow, 0.f, 1.f)));
}

// A radial point-spread image, with faint chromatic aberration instead of a filled disc.
// Both its optical radius and RGB separation collapse with actual collected light only.
static void star_focus(scene_t *s, const sky_star_t *st, float appear, float focus, float guide) {
    if (appear <= .004f) return;
    float blur = 1.f - focus, offset = 2.8f * blur * blur;
    float gain = appear * (.26f + .6f * focus + .025f * guide);
    float radius = 1.1f + 4.2f * blur;
    sc_blur_dot(s, st->x - offset, st->y + offset * .35f, radius, rgb_scale(0xFF9B80, gain));
    sc_blur_dot(s, st->x, st->y - offset * .35f, radius, rgb_scale(0x92FFC8, gain));
    sc_blur_dot(s, st->x + offset, st->y + offset * .35f, radius, rgb_scale(0x97B7FF, gain));
    if (focus > 0) light(s, st->x, st->y, .2f + 2.6f * focus, 0xFFF7E5, appear * (.25f + .75f * focus * focus), .65f);
}

static void star_rays(scene_t *s, const sky_star_t *st, float appear) {
    float strength = clampf((st->fill >= SKY_FULL ? .14f : 0.f) + st->glow * .75f, 0.f, 1.f);
    if (strength <= .01f) return;
    float reach = 6.f + 9.f * st->glow;
    prim_t *p = sc_capsule(s, st->x, st->y - reach, st->x, st->y + reach, 2.f, 0xFFF5E1, appear * strength);
    pr_soft(p, 2.5f);
    p = sc_capsule(s, st->x - reach * .65f, st->y, st->x + reach * .65f, st->y, 1.7f, 0xFFF5E1, appear * strength * .8f);
    pr_soft(p, 2.3f);
}

static void draw_star(scene_t *s, const sky_star_t *st, float appear, float t, bool hinted) {
    float fill = clampf(st->fill / SKY_FULL, 0.f, 1.f);
    float guide = hinted ? .5f + .5f * tess_sin(t * 3.f) : 0.f;
    float gathered = smooth01(fill);
    star_focus(s, st, appear, gathered, guide);
    star_rays(s, st, appear);
}

// A short exposure of selected moving dots, not another simulation or a persistent history buffer.
static void draw_stream_trails(scene_t *s, const tess_sky_t *k, float appear) {
    int trails = 0;
    for (int i = 0; i < SKY_PARTICLES && trails < MAX_STREAM_TRAILS && s->dots_n < R_MAX_DOTS; i += 5) {
        const sky_particle_t *p = &k->p[i];
        int speed = abs(p->vx) + abs(p->vy);
        if (p->wait || speed < 96) continue;
        float gain = clampf((float)speed / (SKY_Q3 * 350.f), 0.f, 1.f);
        float x = ((float)p->x - (float)p->vx * .022f) / SKY_Q3;
        float y = ((float)p->y - (float)p->vy * .022f) / SKY_Q3;
        light(s, x, y, .9f + .35f * gain, k_colour[p->color], appear * (.18f + .15f * gain), .15f);
        trails++;
    }
}

// When the melody first sings star i, ms from its start (-1: never).
static float star_ms(const sky_melody_t *m, int star) {
    for (int i = 0; i < m->n; i++) if (m->note[i].star == star) return sky_melody_start_ms(m, i);
    return -1.f;
}
static float constellation_gap(const sky_world_t *w, const sky_edge_t *edge, int count) {
    float length = 0;
    for (int i = 0; i < count; i++) {
        const sky_star_t *a = &w->star[edge[i].a], *b = &w->star[edge[i].b];
        length += hypotf(b->x - a->x, b->y - a->y);
    }
    return fmaxf(LINE_GAP, length / (float)(MAX_LINE_DOTS - count));
}
// The melody follows the player's order. The picture always follows the chart's real links.
static void draw_constellation(scene_t *s, const tess_sky_t *k, float appear) {
    const sky_world_t *w = &k->w;
    const sky_edge_t *edges;
    int count = sky_constellation_edges(w, &edges), dots = 0;
    float ms = (k->resolve_t - SKY_MELODY_AT_S) * 1000.f;
    float gap = constellation_gap(w, edges, count);
    for (int i = 0; i < count && dots < MAX_LINE_DOTS; i++) {
        int ai = edges[i].a, bi = edges[i].b;
        const sky_star_t *a = &w->star[ai], *b = &w->star[bi];
        float t0 = fmaxf(star_ms(&k->melody, ai), star_ms(&k->melody, bi));
        float reveal = clampf((ms - t0) / 350.f, 0, 1);
        float dx = b->x - a->x, dy = b->y - a->y, len = hypotf(dx, dy);
        int steps = (int)ceilf(len / gap);
        if (steps < 2) steps = 2;
        for (int d = 1; d < steps && dots < MAX_LINE_DOTS; d++, dots++) {
            float u = (float)d / (float)steps, shown = clampf((reveal - u) * 4.f, 0, 1);
            if (shown > 0) light(s, a->x + dx * u, a->y + dy * u, 1.2f, 0xFFD98A, shown * appear * .65f, .5f);
        }
    }
    const char *name = sky_constellation_name(w);
    if (name) sc_label(s, TXT_CAPTION, name, 240, 446, 0, 0xF4D8A3,
                       appear * smooth01(k->resolve_t / .65f));
}

// Warm a vertex already belonging to Tess, without a separate floating object or trail.
static void draw_call(const face_t *f, scene_t *s) {
    float x, y, shown = sky_summon_shown(f);
    if (!tess_sky_calling(f, &x, &y) || shown <= 0) return;
    float breath = .75f + .25f * tess_sin(f->t * .8f);
    light(s, x, y, 2.5f, 0xF4C37B, shown * breath * .22f, .35f);
}

void tess_sky_draw(const face_t *f, scene_t *s) {
    const tess_sky_t *k = f->tess_games.sky;
    if (!k) draw_call(f, s);
    if (!k || f->tess_games.game != TESS_GAME_SKY || k->phase == SKY_UNRAVEL) return;
    const sky_world_t *w = &k->w;
    float appear = smooth01(w->t / 1.2f) * (1.f - smooth01(k->fade / SKY_FADE_S)), time = f->t;
    for (int i = 0; i < w->count_stars; i++)
        draw_star(s, &w->star[i], appear, time, i == k->hint_star);
    if (k->phase == SKY_RESOLVE) draw_constellation(s, k, appear);
    else draw_stream_trails(s, k, appear);
}

// The stream's procedural colour; zero keeps Tess's own turquoise palette.
int tess_sky_dot_colour(const face_t *f, int point) {
    const tess_sky_t *k = f->tess_games.sky;
    return k && f->tess_games.game == TESS_GAME_SKY ? k->p[f->tess_scatter_of[point]].color : 0;
}

uint8_t tess_sky_alpha(const face_t *f) {
    const tess_sky_t *k = f->tess_games.sky;
    // On an ordinary exit Tess gathers these visible dots back into her body.
    // Only a change of sky hides the complete stream before resetting its source.
    if (!k || f->tess_games.game != TESS_GAME_SKY || k->phase != SKY_RESOLVE) return 255;
    return (uint8_t)(255.f * (1.f - smooth01(k->fade / SKY_FADE_S)));
}

bool tess_sky_dot_visible(const face_t *f, int point) {
    const tess_sky_t *k = f->tess_games.sky;
    if (!k || f->tess_games.game != TESS_GAME_SKY || (k->phase != SKY_PLAY && k->phase != SKY_RESOLVE)) return true;
    return !k->p[f->tess_scatter_of[point]].wait;
}
