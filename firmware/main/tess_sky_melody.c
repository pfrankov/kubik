#include "tess_sky_melody.h"
#include <string.h>

#define MIN_SLOTS 5          // star notes in the opening, repeating the tour when the sky has few stars
#define ANSWER_MAX 8
#define LOW_DEGREE 0
#define HIGH_DEGREE 14
#define OCTAVE 5             // steps in one octave of the pentatonic scale
#define LENGTH_LOW_MS 2300   // the whole melody lands inside 2200..4200 ms with a little margin
#define LENGTH_HIGH_MS 4100

typedef struct { uint32_t s; } rng_t;
typedef struct { uint8_t n, u[4]; } cell_t;

// Pleasant rhythm cells, long-short and short-long, tiled over each phrase.
static const cell_t k_cell[] = {
    {2, {3, 1}}, {2, {2, 2}}, {3, {2, 1, 1}}, {3, {1, 1, 2}}, {3, {1, 2, 1}}, {4, {2, 1, 2, 1}}, {2, {1, 3}},
};

static uint32_t rnd(rng_t *r) {
    uint32_t x = r->s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return r->s = x;
}
static int pick(rng_t *r, int n) { return (int)((rnd(r) >> 8) % (uint32_t)n); }
static int clampi(int v, int low, int high) { return v < low ? low : v > high ? high : v; }
static int absi(int v) { return v < 0 ? -v : v; }
static int sign(int v) { return v < 0 ? -1 : v > 0; }

// The generator is seeded from the seed and the world's stars, so two skies never share a tune by accident.
static rng_t seed_rng(const sky_world_t *w, uint32_t seed) {
    uint32_t h = 2166136261u ^ (seed * 2654435761u);
    uint32_t words[3 + 3 * SKY_STARS] = {w->count_stars, w->root, (uint32_t)(int32_t)w->source.x};
    int n = 3;
    for (int i = 0; i < w->count_stars && i < SKY_STARS; i++) {
        words[n++] = w->star[i].degree;
        words[n++] = (uint32_t)(int32_t)w->star[i].x;
        words[n++] = (uint32_t)(int32_t)w->star[i].y;
    }
    for (int i = 0; i < n; i++) { h ^= words[i]; h *= 16777619u; h ^= h >> 15; }
    rng_t r = {h ? h : 0x9e3779b9u};
    for (int i = 0; i < 4; i++) rnd(&r);
    return r;
}

static int star_count(const sky_world_t *w) { return clampi(w->count_stars, 1, SKY_STARS); }
static int star_degree(const sky_world_t *w, int star) { return clampi(w->star[star].degree, 0, 9); }

// The stars in the order they were made full, then those never made full by a nearest-neighbour path from the last.
static void build_tour(const sky_world_t *w, const sky_played_t *played, int8_t tour[SKY_STARS]) {
    int n = star_count(w), k = 0;
    bool used[SKY_STARS] = {false};
    float px = w->source.x, py = w->source.y;
    for (int i = 0; i < SKY_STARS; i++) tour[i] = -1;
    for (int i = 0; i < played->n && i < SKY_STARS; i++) {
        int s = played->order[i];
        if (s < 0 || s >= n || used[s]) continue;
        used[s] = true;
        tour[k++] = (int8_t)s;
        px = w->star[s].x; py = w->star[s].y;
    }
    for (; k < n; k++) {
        int best = -1;
        float best_d = 0;
        for (int i = 0; i < n; i++) {
            float dx = w->star[i].x - px, dy = w->star[i].y - py, d = dx * dx + dy * dy;
            if (!used[i] && (best < 0 || d < best_d)) { best = i; best_d = d; }
        }
        used[best] = true;
        tour[k] = (int8_t)best;
        px = w->star[best].x; py = w->star[best].y;
    }
}
// How long the player worked for a star: its note's length, 1..3 units (0: not known, the rhythm cells decide).
static int worked_units(const sky_played_t *played, int star) {
    for (int i = 0; i < played->n && i < SKY_STARS; i++) {
        if (played->order[i] != star) continue;
        float took = played->at[i] - (i ? played->at[i - 1] : 0.f);
        return took > 9.f ? 3 : took > 3.f ? 2 : 1;
    }
    return 0;
}

// What a leap costs the line: rest is dull, stepwise is sweet, wide is a strain.
static int leap_cost(int leap) {
    if (leap == 0) return 30;
    return leap > 5 ? 100 + leap * leap : leap * leap;
}

// Each star may sing at its own height or an octave up: choose the smoothest line, with a little seeded wobble.
static void pick_octaves(const sky_world_t *w, const int8_t *slot, int n, rng_t *r, int8_t *deg) {
    int cost[SKY_STARS][2];
    int8_t from[SKY_STARS][2];
    for (int i = 0; i < n; i++) {
        int base = star_degree(w, slot[i]);
        for (int c = 0; c < 2; c++) {
            int wobble = pick(r, 6), best = 0, via = 0;
            if (i > 0) {
                best = 1 << 20;
                for (int p = 0; p < 2; p++) {
                    int prev = star_degree(w, slot[i - 1]) + OCTAVE * p;
                    int total = cost[i - 1][p] + leap_cost(absi(base + OCTAVE * c - prev));
                    if (total < best) { best = total; via = p; }
                }
            }
            cost[i][c] = best + wobble;
            from[i][c] = (int8_t)via;
        }
    }
    int c = cost[n - 1][1] < cost[n - 1][0];
    for (int i = n - 1; i >= 0; i--) {
        deg[i] = (int8_t)(star_degree(w, slot[i]) + OCTAVE * c);
        c = from[i][c];
    }
}

// The answer comes home: from the last star note in steps of two or less to a note beside the tonic (a neighbour, or the
// fifth above or below), then the tonic itself. Returns how many notes were written; the last one is the tonic.
static int plan_answer(const sky_world_t *w, int last, rng_t *r, int8_t out[ANSWER_MAX]) {
    int root = clampi(w->root, 0, 4), low = root, high = root + OCTAVE;
    int tonic = absi(last - low) < absi(last - high) ? low : high;
    int offset = pick(r, 2) ? 1 : 3;
    int near = tonic + (last >= tonic ? offset : -offset);
    if (near < LOW_DEGREE || near > HIGH_DEGREE) near = tonic - (near - tonic);
    int cur = last, count = 0;
    while (absi(cur - near) > 2 && count < ANSWER_MAX - 2) {
        cur += 2 * sign(near - cur);
        out[count++] = (int8_t)cur;
    }
    if (count > 0 || near != last) out[count++] = (int8_t)near;
    out[count++] = (int8_t)tonic;
    return count;
}

// How many passing tones go between two star notes `leap` steps apart: wide leaps are bridged, close ones may stay bare.
static int passing_min(int leap) { return leap > 2 ? (leap - 2 < 2 ? leap - 2 : 2) : 0; }
static int passing_max(int leap) { return leap < 1 ? 0 : (leap - 1 < 2 ? leap - 1 : 2); }

static void split_passing(const int8_t *deg, int n, int budget, rng_t *r, int8_t *count) {
    for (int g = 0; g + 1 < n; g++) {
        int need = passing_min(absi(deg[g + 1] - deg[g]));
        count[g] = (int8_t)(need < budget ? need : budget);
        budget -= count[g];
    }
    for (int g = 0; g + 1 < n; g++) {
        int room = passing_max(absi(deg[g + 1] - deg[g])) - count[g];
        int extra = room > 0 ? pick(r, room + 1) : 0;
        extra = extra < budget ? extra : budget;
        count[g] = (int8_t)(count[g] + extra);
        budget -= extra;
    }
}

static void put(sky_melody_t *m, int degree, int star) {
    sky_note_t *note = &m->note[m->n++];
    note->degree = (int8_t)degree;
    note->star = (int8_t)star;
    note->units = 2;
}

// The notes in order. Returns the index where the answer begins.
static int assemble(sky_melody_t *m, const int8_t *slot, const int8_t *deg, int n, const int8_t *answer, int count, rng_t *r) {
    int8_t passing[SKY_STARS] = {0};
    split_passing(deg, n, SKY_MELODY_MAX - n - count, r, passing);
    for (int i = 0; i < n; i++) {
        put(m, deg[i], slot[i]);
        for (int j = 1; i + 1 < n && j <= passing[i]; j++) put(m, deg[i] + j * sign(deg[i + 1] - deg[i]), -1);
    }
    int split = m->n;
    for (int i = 0; i < count; i++) put(m, answer[i], -1);
    return split;
}

static void shorten(sky_melody_t *m, int high, int start) {
    for (int g = 0; g < 3 * m->n && m->total_units > high; g++) {
        sky_note_t *note = &m->note[(start + g) % (m->n - 1)];
        if (note->units > 1) { note->units--; m->total_units--; }
    }
    sky_note_t *tonic = &m->note[m->n - 1];
    while (m->total_units > high && tonic->units > 4) { tonic->units--; m->total_units--; }
}

static void lengthen(sky_melody_t *m, int low, int start) {
    sky_note_t *tonic = &m->note[m->n - 1];
    while (m->total_units < low && tonic->units < 8) { tonic->units++; m->total_units++; }
    for (int g = 0; g < 3 * m->n && m->total_units < low; g++) {
        sky_note_t *note = &m->note[(start + g) % (m->n - 1)];
        if (note->units < 3) { note->units++; m->total_units++; }
    }
}

// Long-short cells over each phrase, the closing note held longest, the whole thing fitted to 2.3..4.1 seconds.
static void set_rhythm(sky_melody_t *m, const sky_played_t *played, int split, rng_t *r) {
    const cell_t *cell = &k_cell[pick(r, (int)(sizeof k_cell / sizeof k_cell[0]))];
    int at = 0;
    m->total_units = 0;
    for (int i = 0; i < m->n; i++) {
        if (i == split) { cell = &k_cell[pick(r, (int)(sizeof k_cell / sizeof k_cell[0]))]; at = 0; }
        m->note[i].units = cell->u[at++ % cell->n];
        int worked = m->note[i].star >= 0 && i < split ? worked_units(played, m->note[i].star) : 0;
        if (worked) m->note[i].units = (uint8_t)worked;
        m->total_units = (uint8_t)(m->total_units + m->note[i].units);
    }
    sky_note_t *tonic = &m->note[m->n - 1];
    int held = 4 + pick(r, 3);
    m->total_units = (uint8_t)(m->total_units - tonic->units + held);
    tonic->units = (uint8_t)held;
    int low = (LENGTH_LOW_MS + m->unit_ms - 1) / m->unit_ms, high = LENGTH_HIGH_MS / m->unit_ms;
    shorten(m, high, pick(r, m->n - 1));
    lengthen(m, low, pick(r, m->n - 1));
}

// Phrase starts and star notes lean in; passing tones are light; the home note is the fullest.
static void set_accents(sky_melody_t *m, int split, rng_t *r) {
    for (int i = 0; i < m->n; i++) {
        int accent = m->note[i].star >= 0 ? 165 : 85;
        if (i == 0 || i == split) accent = 215;
        m->note[i].accent = (uint8_t)(accent + pick(r, 20));
    }
    m->note[m->n - 1].accent = 255;
}
// Free play has no target or imaginary star tour: a short descending phrase simply brings it home.
static void aurora_melody(const sky_world_t *w, rng_t *r, sky_melody_t *m) {
    int root = clampi(w->root, 0, 4);
    for (int i = 0; i < SKY_STARS; i++) m->tour[i] = -1;
    m->n = 4; m->total_units = 14; m->unit_ms = (uint16_t)(170 + pick(r, 30));
    for (int i = 0; i < 4; i++)
        m->note[i] = (sky_note_t){.degree=(int8_t)(root+3-i),.star=-1,.units=(uint8_t)(i==3?8:2),.accent=(uint8_t)(i==3?255:160)};
    m->note[0].degree += (int8_t)pick(r, 2);
    m->chord[0]=(int8_t)root; m->chord[1]=(int8_t)(root+2); m->chord[2]=(int8_t)(root+3);
}

void sky_melody_compose(const sky_world_t *w, const sky_played_t *played, uint32_t seed, sky_melody_t *m) {
    memset(m, 0, sizeof *m);
    rng_t r = seed_rng(w, seed);
    if (!w->count_stars) { aurora_melody(w, &r, m); return; }
    int stars = star_count(w), slots = stars < MIN_SLOTS ? MIN_SLOTS : stars;
    int8_t slot[SKY_STARS], deg[SKY_STARS], answer[ANSWER_MAX];
    build_tour(w, played, m->tour);
    for (int i = 0; i < slots; i++) slot[i] = m->tour[i % stars];
    pick_octaves(w, slot, slots, &r, deg);
    int count = plan_answer(w, deg[slots - 1], &r, answer);
    int split = assemble(m, slot, deg, slots, answer, count, &r);
    m->unit_ms = (uint16_t)(140 + pick(&r, 61));
    set_rhythm(m, played, split, &r);
    set_accents(m, split, &r);
    int root = clampi(w->root, 0, 4);
    m->chord[0] = (int8_t)root;
    m->chord[1] = (int8_t)clampi(root + 2, 0, HIGH_DEGREE);
    m->chord[2] = (int8_t)clampi(root + 3, 0, HIGH_DEGREE);
}

float sky_melody_start_ms(const sky_melody_t *m, int i) {
    int units = 0;
    for (int j = 0; j < i && j < m->n; j++) units += m->note[j].units;
    return (float)(units * m->unit_ms);
}

float sky_melody_length_ms(const sky_melody_t *m) { return (float)(m->total_units * m->unit_ms); }
