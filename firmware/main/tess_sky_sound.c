#include "tess_sky_play.h"

// What Skies asks of the speaker: the music grows a layer at a time from what happens on the glass. Light flowing into
// a star sparkles softly, climbing its note's scale as it fills; a star that becomes full rings its note (a relay a fifth
// more, as it sends the light on) and from then on holds it as a pad whose gain follows its fill, and pulses it again
// whenever light flows through it, quicker the more flows: every full star is one more voice. A well hums under the
// others while it holds light. Light striking a string sounds one of the sky's own star notes, in the middle of the
// range (the longer string, the lower), soft and rate limited; a relay turned and a tool put in its place click softly.
// Nothing is on a grid. On resolving the sky sings back how it was played (tess_sky_melody.c), each star flashing as its
// note comes, and closes on its chord.
#define REFRESH_S .25f
#define MOVE .05f
#define PAD_BIT(i) (1u << (i))
#define HUM_BIT(i) (1u << (SKY_STARS + (i)))
#define HUM_FULL 14.f   // particles circling a well for its full hum

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
#define STRIKE_GAP_S .14f
#define PULSE_SLOW_S 1.5f   // a full star's pulse with a trickle through it; a strong flow halves it
static int in_range(int degree) { while (degree > TOP_DEGREE) degree -= 5; return degree; }
// The sky's star note nearest the string's own place in the middle of the range (degrees 2..8: the longer, the lower).
static float string_hz(const sky_world_t *w, const sky_tool_t *t) {
    float want = 8.f - clampf((2.f * hypotf(t->dx, t->dy) - 60.f) / 20.f, 0.f, 6.f);
    int best = (int)want;
    float best_d = 99.f;
    for (int i = 0; i < w->count_stars; i++)
        for (int d = w->star[i].degree % 5; d <= 9; d += 5) {
            float gap = fabsf((float)d - want);
            if (gap < best_d) { best_d = gap; best = d; }
        }
    return sky_degree_hz(best);
}

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
    tess_tone(f, TT_SKY_PLUCK, note_hz(s->degree + (int)(s->fill / SKY_FULL * 4.f)), .1f + .14f * s->fill, 1.f);
    k->sparkle_wait = .16f + .1f * frand(f);
}
// A full star with light flowing through it pulses its note: one voice more for every full star.
static void pulse(face_t *f, tess_sky_t *k, const sky_hits_t *hits, float dt) {
    for (int i = 0; i < k->w.count_stars; i++) {
        const sky_star_t *s = &k->w.star[i];
        k->pulse_wait[i] -= dt;
        if (k->phase != SKY_PLAY || s->fill < SKY_FULL || hits->inside[i] < 1 || k->pulse_wait[i] > 0) continue;
        float flow = fminf(1.f, (float)hits->inside[i] / 10.f);
        tess_tone(f, TT_SKY_PLUCK, note_hz(s->degree + (i & 1 ? 5 : 0)), .14f + .1f * flow, .7f);
        k->pulse_wait[i] = PULSE_SLOW_S * (1.f - .5f * flow) * (1.f + .13f * (float)i);   // (each its own time: no grid)
    }
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
            tess_tone(f, TT_SKY_PLUCK, note_hz(s->degree), .5f, .3f);
            tess_tone_later(f, TT_SKY_PLUCK, note_hz(s->degree + 5), .3f, .5f, .09f);
            if (s->relay) tess_tone_later(f, TT_SKY_PLUCK, note_hz(s->degree + 3), .26f, .5f, .19f);
            k->pulse_wait[i] = .8f;
        }
        if (k->turns & (1u << i)) tess_tone(f, TT_SKY_PLUCK, note_hz(s->degree), .24f, .9f);
    }
    if (k->snaps) {
        tess_tone(f, TT_SKY_PLUCK, note_hz(w->root + 5), .24f, 1.f);
        tess_tone_later(f, TT_SKY_PLUCK, note_hz(w->root + 7), .2f, 1.f, .06f);
    }
    k->snaps = k->turns = 0;
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
        bool on = send(f, TT_SKY_PAD, i, s->hz, pad_level(s->fill) * fade, s->color * .5f, &k->pad_gain[i], &k->pad_sent[i], dt, g->sky_lit & PAD_BIT(i));
        g->sky_lit = (uint16_t)(on ? g->sky_lit | PAD_BIT(i) : g->sky_lit & ~PAD_BIT(i));
    }
    float hum_hz = sky_degree_hz(w->root) * .5f;
    for (int i = 0; i < w->count_tools; i++) {
        sky_tool_t *t = &w->tool[i];
        k->strike_wait[i] -= dt;
        if (t->kind == TOOL_STRING && hits->strike[i] > 0) t->shown = 1.f;
        if (t->kind == TOOL_STRING && hits->strike[i] > 0 && k->strike_wait[i] <= 0) {
            tess_tone(f, TT_SKY_PLUCK, string_hz(w, t), .16f + .26f * hits->strike[i], .75f);
            k->strike_wait[i] = STRIKE_GAP_S;
        }
        if (t->kind != TOOL_WELL) continue;
        bool on = send(f, TT_SKY_HUM, i, hum_hz, fminf(1.f, (float)t->held / HUM_FULL) * fade, 0, &k->hum_gain[i], &k->hum_sent[i], dt, g->sky_lit & HUM_BIT(i));
        g->sky_lit = (uint16_t)(on ? g->sky_lit | HUM_BIT(i) : g->sky_lit & ~HUM_BIT(i));
    }
}

// The sky resolves: a soft swell on its root (its melody follows, a note at a time).
void sky_sound_resolve(face_t *f, tess_sky_t *k) {
    tess_tone(f, TT_SKY_CHIME, sky_degree_hz(k->w.root), .4f, .3f);
}
// The melody's notes that are due by `ms` from its start; a star whose note it is flashes, and after the last the
// sky's chord rings under the tonic.
void sky_sound_melody(face_t *f, tess_sky_t *k, float ms) {
    const sky_melody_t *m = &k->melody;
    while (k->melody_next < m->n && ms >= sky_melody_start_ms(m, k->melody_next)) {
        const sky_note_t *n = &m->note[k->melody_next++];
        float shade = clampf(1.f - (float)n->units / 4.f, 0.f, 1.f);
        tess_tone(f, TT_SKY_PLUCK, note_hz(n->degree), .3f + .42f * (float)n->accent / 255.f, shade);
        if (n->star >= 0) k->w.star[n->star].glow = 1.f;
        if (n->star >= 0 && k->tour_lit < SKY_STARS && m->tour[k->tour_lit] == n->star) k->tour_lit++;
        if (k->melody_next < m->n) continue;
        for (int c = 0; c < 3; c++) tess_tone_later(f, TT_SKY_CHIME, note_hz(m->chord[c]), .4f, .6f, .03f * (float)c);
        tess_tone(f, TT_SKY_CHIME, sky_degree_hz(m->chord[0] % 5) * .5f, .45f, .9f);
    }
}

void sky_sound_silence(face_t *f) {
    tess_games_t *g = &f->tess_games;
    for (int i = 0; i < SKY_STARS; i++)
        if (g->sky_lit & PAD_BIT(i)) tess_tone_slot(f, TT_SKY_PAD, (unsigned)i, 440.f, 0, 0, 0);
    for (int i = 0; i < SKY_TOOLS; i++)
        if (g->sky_lit & HUM_BIT(i)) tess_tone_slot(f, TT_SKY_HUM, (unsigned)i, 110.f, 0, 0, 0);
    g->sky_lit = 0;
}
