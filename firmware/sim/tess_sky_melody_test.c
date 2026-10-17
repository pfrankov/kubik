// Host test of the ending melody: every musical rule over many random worlds and seeds.
#include "tess_sky_internal.h"
#include "tess_sky_melody.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WORLDS 200
#define SEEDS 50
#define VARIETY 200

static uint32_t rng = 12345u;
static int rnd(int n) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (int)((rng >> 8) % (uint32_t)n);
}

static void make_world(sky_world_t *w, int stars) {
    memset(w, 0, sizeof *w);
    w->count_stars = (uint8_t)stars;
    w->root = (uint8_t)rnd(5);
    w->source.x = (float)(46 + rnd(388));
    w->source.y = (float)(60 + rnd(374));
    for (int i = 0; i < stars; i++) {
        w->star[i].x = (float)(46 + rnd(388));
        w->star[i].y = (float)(60 + rnd(374));
        w->star[i].degree = (uint8_t)rnd(10);
        w->star[i].hz = sky_degree_hz(w->star[i].degree);
    }
}

// The player lit some of the stars, in a random order, `pace` s apart (none when `pace` is 0).
static sky_played_t make_played(const sky_world_t *w, float pace) {
    sky_played_t p = {0};
    bool used[SKY_STARS] = {false};
    int lit = pace > 0 ? 1 + rnd(w->count_stars) : 0;
    for (int i = 0; i < lit; i++) {
        int s = rnd(w->count_stars);
        while (used[s]) s = (s + 1) % w->count_stars;
        used[s] = true;
        p.order[p.n] = (int8_t)s;
        p.at[p.n] = (p.n ? p.at[p.n - 1] : 0.f) + pace;
        p.n++;
    }
    return p;
}

static int iabs(int v) { return v < 0 ? -v : v; }

// The tour is the order the stars were lit in, then the rest nearest first from the last.
static void check_tour(const sky_world_t *w, const sky_played_t *p, const sky_melody_t *m) {
    bool used[SKY_STARS] = {false};
    float px = w->source.x, py = w->source.y;
    for (int k = 0; k < SKY_STARS; k++) {
        if (k >= w->count_stars) { assert(m->tour[k] == -1); continue; }
        if (k < p->n) {
            assert(m->tour[k] == p->order[k]);
            used[p->order[k]] = true;
            px = w->star[p->order[k]].x; py = w->star[p->order[k]].y;
            continue;
        }
        int best = -1;
        float best_d = 0;
        for (int i = 0; i < w->count_stars; i++) {
            float dx = w->star[i].x - px, dy = w->star[i].y - py, d = dx * dx + dy * dy;
            if (!used[i] && (best < 0 || d < best_d)) { best = i; best_d = d; }
        }
        assert(m->tour[k] == best);
        used[best] = true;
        px = w->star[best].x; py = w->star[best].y;
    }
}

// Every star is sung (first times in tour order), at its own height or an octave above.
static void check_stars(const sky_world_t *w, const sky_melody_t *m) {
    int seen = 0;
    for (int i = 0; i < m->n; i++) {
        int s = m->note[i].star;
        if (s < 0) continue;
        assert(s < w->count_stars);
        int d = w->star[s].degree;
        assert(m->note[i].degree == d || m->note[i].degree == d + 5);
        assert(m->note[i].degree <= 14);
        bool first = true;
        for (int j = 0; j < i; j++) if (m->note[j].star == s) first = false;
        if (first) assert(m->tour[seen++] == s);
    }
    assert(seen == w->count_stars);
}

// Between two star notes: 0..2 passing tones, each one step along the way to the next star note.
static void check_passing(const sky_melody_t *m) {
    int last_star = -1;
    for (int i = 0; i < m->n; i++) if (m->note[i].star >= 0) last_star = i;
    int prev = -1;
    for (int i = 0; i <= last_star; i++) {
        if (m->note[i].star < 0) continue;
        if (prev >= 0) {
            int gap = i - prev - 1, way = m->note[i].degree - m->note[prev].degree;
            assert(gap >= 0 && gap <= 2);
            for (int j = prev + 1; j < i; j++) {
                int step = m->note[j].degree - m->note[j - 1].degree;
                assert(iabs(step) == 1 && (step > 0) == (way > 0));
            }
        }
        prev = i;
    }
    for (int i = last_star + 1; i < m->n; i++) assert(m->note[i].star == -1);
}

static void check_ending(const sky_world_t *w, const sky_melody_t *m) {
    const sky_note_t *last = &m->note[m->n - 1], *near = &m->note[m->n - 2];
    assert(last->degree == w->root || last->degree == w->root + 5);
    assert(last->units >= 4 && last->units <= 8);
    for (int i = 0; i + 1 < m->n; i++) assert(m->note[i].units < last->units);
    int gap = iabs(near->degree - last->degree);
    assert(gap <= 1 || gap == 3);
    assert(m->chord[0] == w->root && m->chord[1] == w->root + 2 && m->chord[2] == w->root + 3);
}

static int check_line(const sky_melody_t *m, int *small) {
    int leaps = 0;
    for (int i = 0; i < m->n; i++) {
        assert(m->note[i].degree >= 0 && m->note[i].degree <= 14);
        assert(m->note[i].units >= 1 && m->note[i].units <= 8);
        if (i == 0) continue;
        int leap = iabs(m->note[i].degree - m->note[i - 1].degree);
        assert(leap <= 5);
        leaps++;
        if (leap <= 2) (*small)++;
        if (i >= 2) assert(!(m->note[i].degree == m->note[i - 1].degree && m->note[i].degree == m->note[i - 2].degree));
    }
    return leaps;
}

static void check_shape(const sky_melody_t *m) {
    assert(m->n >= 6 && m->n <= SKY_MELODY_MAX);
    assert(m->unit_ms >= 140 && m->unit_ms <= 200);
    int sum = 0;
    for (int i = 0; i < m->n; i++) {
        sum += m->note[i].units;
        assert(fabsf(sky_melody_start_ms(m, i + 1) - sky_melody_start_ms(m, i) - (float)(m->note[i].units * m->unit_ms)) < .5f);
        // Passing tones are light, star notes lean in (a phrase start leans in either way).
        if (m->note[i].star >= 0) assert(m->note[i].accent >= 165);
        else if (i > 0 && i < m->n - 1) assert(m->note[i].accent < 110 || m->note[i].accent >= 215);
    }
    assert(sum == m->total_units);
    assert(sky_melody_length_ms(m) >= 2200.f && sky_melody_length_ms(m) <= 4200.f);
}

static int distinct(const sky_melody_t *list, int n) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) seen = memcmp(&list[i], &list[j], sizeof list[i]) == 0;
        count += !seen;
    }
    return count;
}

int main(void) {
    static sky_melody_t list[VARIETY];
    int leaps = 0, small = 0;
    for (int k = 0; k < WORLDS; k++) {
        sky_world_t w;
        sky_world_t before;
        make_world(&w, 1 + k % SKY_STARS);
        before = w;
        for (int s = 0; s < SEEDS; s++) {
            sky_melody_t m, again;
            uint32_t seed = (uint32_t)rnd(1 << 30) * 7u + (uint32_t)s;
            sky_played_t played = make_played(&w, s % 2 ? (float)(1 + rnd(12)) : 0.f);
            sky_melody_compose(&w, &played, seed, &m);
            sky_melody_compose(&w, &played, seed, &again);
            assert(memcmp(&m, &again, sizeof m) == 0);
            assert(memcmp(&w, &before, sizeof w) == 0);
            check_tour(&w, &played, &m);
            check_stars(&w, &m);
            check_passing(&m);
            check_ending(&w, &m);
            check_shape(&m);
            leaps += check_line(&m, &small);
        }
        // A sky with a single star still makes a full melody.
        if (k < SKY_STARS) {
            make_world(&w, 1);
            sky_melody_t one;
            sky_melody_compose(&w, &(sky_played_t){0}, 1u + (uint32_t)k, &one);
            assert(one.n >= 6);
        }
    }
    sky_world_t w;
    make_world(&w, 4);
    for (int s = 0; s < VARIETY; s++) sky_melody_compose(&w, &(sky_played_t){0}, (uint32_t)s, &list[s]);
    int seeds_distinct = distinct(list, VARIETY);
    assert(seeds_distinct >= 150);
    for (int k = 0; k < VARIETY; k++) {
        make_world(&w, 1 + k % SKY_STARS);
        sky_melody_compose(&w, &(sky_played_t){0}, 7u, &list[k]);
    }
    assert(distinct(list, VARIETY) >= 150);
    // Stars that took long to light are sung longer than stars lit at once.
    int slow_units = 0, quick_units = 0, slow_n = 0, quick_n = 0;
    for (int k = 0; k < WORLDS; k++) {
        make_world(&w, 2 + k % (SKY_STARS - 1));
        sky_played_t slow = make_played(&w, 12.f), quick = slow;
        for (int i = 0; i < quick.n; i++) quick.at[i] = .5f * (float)(i + 1);
        sky_melody_t a, b;
        sky_melody_compose(&w, &slow, 3u, &a);
        sky_melody_compose(&w, &quick, 3u, &b);
        for (int i = 0; i < a.n; i++) if (a.note[i].star >= 0) { slow_units += a.note[i].units; slow_n++; }
        for (int i = 0; i < b.n; i++) if (b.note[i].star >= 0) { quick_units += b.note[i].units; quick_n++; }
    }
    assert(slow_units * quick_n > quick_units * slow_n * 5 / 4);
    int share = small * 100 / leaps;
    assert(share >= 70);
    printf("sky melody: %d worlds x %d seeds, distinct %d, small-interval share %d%%\n", WORLDS, SEEDS, seeds_distinct, share);
    return 0;
}
