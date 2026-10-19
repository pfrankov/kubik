#include "tess_sky_play.h"
#include <string.h>

// The melody grows with the player's progress. Each newly lit star joins the sustained harmony, and contacts
// add quiet, rate-limited notes. Permanent and temporary black holes hum while the stream circles them.
// Resolution sings back the order in which stars were lit; every slot is released on exit or a new sky.
#define REFRESH_S .25f
#define MOVE .05f
#define PAD_BIT(i) (1u << (i))
#define HUM_BIT(i) (1u << (SKY_STARS + (i)))
#define HUM_FULL 14.f   // particles circling a black hole for its full hum

static float pad_level(float fill) {
    float x = fill < .04f ? 0.f : (fill - .04f) / .96f;
    return x * sqrtf(x);
}
// Sends a voice's gain when it has moved or is due; returns the new "sounding" bit.
static bool send(face_t *f, tess_tone_kind_t kind, int slot, float hz, float level, float shade, float *told, float *since, float dt, bool lit) {
    *since += dt;
    bool due = fabsf(level - *told) > MOVE || (level > 0 && *since >= REFRESH_S) || (level <= 0 && lit && *told > 0);
    if (!due) return lit;
    tess_tone_slot(f, kind, (unsigned)slot, hz, level, shade, 0);
    *told = level; *since = 0;
    return level > 0;
}
#define TOP_DEGREE 11       // E6: nothing in play sounds higher
// Fold without powf in the frame loop: D3..B3, retaining each star's pitch class.
static float low_hz(float hz) { while (hz > 500.f) hz *= .5f; return hz * .5f; }
static int in_range(int degree) { while (degree > TOP_DEGREE) degree -= 5; return degree; }
static float note_hz(int degree) { return sky_degree_hz(in_range(degree)); }
// The busiest star still filling sparkles, rate limited, climbing the scale with its fill.
static void sparkle(face_t *f, tess_sky_t *k, const sky_hits_t *hits, float dt) {
    k->sparkle_wait = fmaxf(0, k->sparkle_wait - dt);
    if (k->phase != SKY_PLAY || k->sparkle_wait > 0) return;
    int best = -1;
    for (int i = 0; i < k->w.count_stars; i++)
        if (k->w.star[i].fill < SKY_FULL && hits->inside[i] > (best < 0 ? 2 : hits->inside[best])) best = i;
    if (best < 0) return;
    const sky_star_t *s = &k->w.star[best];
    tess_tone(f, TT_SKY_GAME_PLUCK, note_hz(s->degree + (int)(s->fill / SKY_FULL * 4.f)), .08f + .1f * s->fill, 1.f);
    k->sparkle_wait = .28f + .16f * frand(f);
}
static unsigned minimum_crossings(const sky_star_t *star) { return star->fill >= SKY_FULL ? 1u : 3u; }
// Passing light forms a sparse 3-2-3 rhythm. Empty beats stay empty; only one star leads a beat.
static void pulse(face_t *f, tess_sky_t *k, const sky_hits_t *hits, float dt) {
    if (k->phase != SKY_PLAY) return;
    for (int i = 0; i < k->w.count_stars; i++)
        if (hits->inside[i] > k->beat_flow[i]) k->beat_flow[i] = hits->inside[i];
    k->beat_wait -= dt;
    if (k->beat_wait > 0) return;
    k->beat_wait = fmaxf(0, k->beat_wait + .28f + .015f * k->w.root);
    unsigned step = k->beat_step++ & 7u;
    int best = -1;
    for (int n = 0; n < k->w.count_stars; n++) {
        int i = (n + step) % k->w.count_stars;
        if (k->beat_flow[i] >= minimum_crossings(&k->w.star[i]) && (best < 0 || k->beat_flow[i] > k->beat_flow[best])) best = i;
    }
    if ((step == 0 || step == 3 || step == 5) && best >= 0) {
        sky_star_t *star = &k->w.star[best];
        float flow = fminf(1.f, (float)k->beat_flow[best] / 12.f);
        tess_tone(f, TT_SKY_GAME_PULSE, low_hz(star->hz), .28f + .24f * flow, (float)(step != 0));
        star->glow = fmaxf(star->glow, .3f + .35f * flow);
    }
    memset(k->beat_flow, 0, sizeof k->beat_flow);
}
// How the sky was played: the stars in the order they first became full, and when.
static void note_played(tess_sky_t *k, int star) {
    sky_played_t *p = &k->played;
    for (int i = 0; i < p->n; i++) if (p->order[i] == star) return;
    if (p->n >= SKY_STARS) return;
    p->order[p->n] = (int8_t)star;
    p->at[p->n++] = k->struggle;
}
static void events(face_t *f, tess_sky_t *k, const sky_hits_t *hits) {
    const sky_world_t *w = &k->w;
    for (int i = 0; k->phase == SKY_PLAY && i < w->count_stars; i++) {
        const sky_star_t *s = &w->star[i];
        if (hits->locked & (1u << i)) {
            note_played(k, i);
            tess_tone(f, TT_SKY_GAME_PLUCK, note_hz(s->degree), .5f, .3f);
            tess_tone_later(f, TT_SKY_GAME_PLUCK, note_hz(s->degree + 5), .3f, .5f, .09f);
        }
    }
}

void sky_sound_frame(face_t *f, tess_sky_t *k, const sky_hits_t *hits, float dt) {
    tess_games_t *g = &f->tess_games;
    sparkle(f, k, hits, dt);
    pulse(f, k, hits, dt);
    events(f, k, hits);
    sky_world_t *w = &k->w;
    float fade = 1.f - smooth01(k->fade / SKY_FADE_S);
    for (int i = 0; i < w->count_stars; i++) {
        const sky_star_t *s = &w->star[i];
        bool on = send(f, TT_SKY_PAD, i, low_hz(s->hz) * (i & 1 ? 2.f : 1.f), pad_level(s->fill) * fade, s->color * .5f, &k->pad_gain[i], &k->pad_sent[i], dt, g->sky_lit & PAD_BIT(i));
        g->sky_lit = (uint16_t)(on ? g->sky_lit | PAD_BIT(i) : g->sky_lit & ~PAD_BIT(i));
    }
    float hum_hz = sky_degree_hz(w->root) * .5f;
    for (int i = 0; i < SKY_HUMS; i++) {
        float gain = fminf(1.f, (float)hits->orbit[i] / HUM_FULL) * fade;
        bool on = send(f, TT_SKY_HUM, i, hum_hz, gain, 0, &k->hum_gain[i], &k->hum_sent[i], dt, g->sky_lit & HUM_BIT(i));
        g->sky_lit = (uint16_t)(on ? g->sky_lit | HUM_BIT(i) : g->sky_lit & ~HUM_BIT(i));
    }
}

// The sky resolves: a soft swell on its root (its melody follows, a note at a time).
void sky_sound_resolve(face_t *f, tess_sky_t *k) {
    tess_tone(f, TT_SKY_GAME_CHIME, sky_degree_hz(k->w.root) * .5f, .4f, .3f);
}
// The melody's notes that are due by `ms` from its start; a star whose note it is flashes, and after the last the
// sky's chord rings under the tonic.
void sky_sound_melody(face_t *f, tess_sky_t *k, float ms) {
    const sky_melody_t *m = &k->melody;
    while (k->melody_next < m->n && ms >= sky_melody_start_ms(m, k->melody_next)) {
        const sky_note_t *n = &m->note[k->melody_next++];
        float shade = clampf(1.f - (float)n->units / 4.f, 0.f, 1.f);
        tess_tone(f, TT_SKY_GAME_PLUCK, note_hz(n->degree), .3f + .42f * (float)n->accent / 255.f, shade);
        if (n->star >= 0) k->w.star[n->star].glow = 1.f;
        if (n->star >= 0 && k->tour_lit < SKY_STARS && m->tour[k->tour_lit] == n->star) k->tour_lit++;
        if (k->melody_next < m->n) continue;
        for (int c = 0; c < 3; c++) tess_tone_later(f, TT_SKY_GAME_CHIME, note_hz(m->chord[c]), .4f, .6f, .03f * (float)c);
        tess_tone(f, TT_SKY_GAME_CHIME, sky_degree_hz(m->chord[0] % 5) * .5f, .45f, .9f);
    }
}

void sky_sound_silence(face_t *f) {
    tess_games_t *g = &f->tess_games;
    for (int i = 0; i < SKY_STARS; i++)
        if (g->sky_lit & PAD_BIT(i)) tess_tone_slot(f, TT_SKY_PAD, (unsigned)i, 440.f, 0, 0, 0);
    for (int i = 0; i < SKY_HUMS; i++)
        if (g->sky_lit & HUM_BIT(i)) tess_tone_slot(f, TT_SKY_HUM, (unsigned)i, 110.f, 0, 0, 0);
    g->sky_lit = 0;
}

// A real exit is distinct from make_sky's held-slot reset: keep a just-resolved chord
// ringing across the next sky, but release every voice owned by this round when it ends.
void sky_sound_exit(face_t *f) {
    sky_sound_silence(f);
    tess_tone(f, TT_SKY_RELEASE, 440.f, 0.f, 0.f);
    f->tess_games.sky_audio = false;
}
