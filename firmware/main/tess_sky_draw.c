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

#define STAR_DUST 6
static const float k_dust[STAR_DUST][2] = {{-.74f, -.26f}, {-.28f, .61f}, {.53f, .42f},
                                        {.82f, -.36f}, {-.16f, -.86f}, {.12f, .17f}};

// A loose, irregular formation contracts with real progress. Time only stirs it: an unfed star cannot appear
// to keep filling. As the last grains meet the core they disappear inside its light, including in RESOLVE.
static void star_dust(scene_t *s, const sky_star_t *st, float appear, float t, float fill, float guide) {
    float open = 1.f - fill * fill;
    if (open <= .001f) return;
    float seed = st->ax * .031f + st->ay * .019f + st->degree * .73f;
    float turn = seed + t * .14f + fill * 1.8f, cs = tess_sin(turn + PI * .5f), sn = tess_sin(turn);
    float spread = SKY_ZONE_R * open;
    for (int i = 0; i < STAR_DUST; i++) {
        float breath = tess_sin(t * .8f + seed + (float)i * 1.7f);
        float x = k_dust[i][0] * spread, y = k_dust[i][1] * spread;
        float drift = 1.f + .08f * breath;
        float shown = appear * (.3f + .4f * fill + .17f * guide) * fminf(1.f, open * 5.f);
        light(s, st->x + (x * cs - y * sn) * drift, st->y + (x * sn + y * cs) * drift,
              1.3f + .6f * fill + .15f * breath, k_colour[st->color], shown, .6f + .35f * fill);
    }
}

static void star_rays(scene_t *s, const sky_star_t *st, float appear, float pulse) {
    float strength = clampf((st->fill >= SKY_FULL ? .25f + .08f * pulse : 0.f) + st->glow * .75f, 0.f, 1.f);
    if (strength <= .01f) return;
    float reach = 6.f + 9.f * st->glow;
    prim_t *p = sc_capsule(s, st->x, st->y - reach, st->x, st->y + reach, 2.f, 0xFFF5E1, appear * strength);
    pr_soft(p, 2.5f);
    p = sc_capsule(s, st->x - reach * .65f, st->y, st->x + reach * .65f, st->y, 1.7f, 0xFFF5E1, appear * strength * .8f);
    pr_soft(p, 2.3f);
}

static void draw_star(scene_t *s, const sky_star_t *st, float appear, float t, bool hinted) {
    uint32_t c = k_colour[st->color];
    float pulse = .5f + .5f * tess_sin(t * 2.2f + st->phase), fill = clampf(st->fill / SKY_FULL, 0.f, 1.f);
    float flash = clampf(st->glow, 0.f, 1.f), guide = hinted ? .5f + .5f * tess_sin(t * 3.f) : 0.f;
    float gathered = smooth01(fill);
    star_dust(s, st, appear, t, fill, guide);
    light(s, st->x, st->y, 3.2f + 3.5f * gathered + 2.f * flash, c,
          appear * (.22f + .4f * gathered + .12f * flash + .08f * guide), .8f + .2f * gathered);
    light(s, st->x, st->y, 1.6f + 2.3f * gathered + .7f * flash,
          palette_mix(c, 0xFFF7E5, .15f + .65f * gathered + .2f * flash),
          appear * (.65f + .35f * gathered), .5f + .5f * gathered);
    star_rays(s, st, appear, pulse);
}

static void draw_source(scene_t *s, const sky_source_t *src, float appear, float t) {
    light(s, src->x, src->y, 4.f + .6f * tess_sin(t * 2.f), 0x74E3CB, appear * .75f, .75f);
    light(s, src->x + src->dx * 2.f, src->y + src->dy * 2.f, 1.5f, 0xE9FFF1, appear, .3f);
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
static float constellation_gap(const tess_sky_t *k) {
    float length = 0;
    int links = 0;
    for (int i = 0; i + 1 < k->w.count_stars && k->melody.tour[i + 1] >= 0; i++) {
        const sky_star_t *a = &k->w.star[k->melody.tour[i]], *b = &k->w.star[k->melody.tour[i + 1]];
        length += hypotf(b->x - a->x, b->y - a->y);
        links++;
    }
    return fmaxf(LINE_GAP, length / (float)(MAX_LINE_DOTS + links));
}
// A line of dots from star to star along the melody's tour: each stretch is drawn out while the melody walks from one
// star's note to the next, and arrives with it. The gap adapts to the whole tour, so its last link is never omitted.
static void draw_constellation(scene_t *s, const tess_sky_t *k) {
    const sky_world_t *w = &k->w;
    const sky_melody_t *m = &k->melody;
    float ms = (k->resolve_t - SKY_MELODY_AT_S) * 1000.f, fade = 1.f - smooth01((k->resolve_t - (k->resolve_len - 1.f)) / .9f);
    int dots = 0;
    float gap = constellation_gap(k);
    for (int i = 0; i + 1 < w->count_stars && m->tour[i + 1] >= 0 && dots < MAX_LINE_DOTS; i++) {
        const sky_star_t *a = &w->star[m->tour[i]], *b = &w->star[m->tour[i + 1]];
        float t0 = star_ms(m, m->tour[i]), t1 = star_ms(m, m->tour[i + 1]);
        float reveal = t0 < 0 || t1 <= t0 ? 0.f : clampf((ms - t0) / (t1 - t0), 0.f, 1.f);
        float dx = b->x - a->x, dy = b->y - a->y, len = hypotf(dx, dy);
        int steps = (int)(len / gap);
        for (int d = 1; d < steps && dots < MAX_LINE_DOTS; d++, dots++) {
            float u = (float)d / (float)steps, shown = clampf((reveal - u) * 4.f, 0, 1);
            if (shown > 0) light(s, a->x + dx * u, a->y + dy * u, 1.5f + .5f * shown, 0xFFD98A, shown * fade, .6f);
        }
    }
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
    draw_source(s, &w->source, appear, time);
    for (int i = 0; i < w->count_stars; i++)
        draw_star(s, &w->star[i], appear, time, i == k->hint_star);
    if (k->phase == SKY_RESOLVE) draw_constellation(s, k);
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
