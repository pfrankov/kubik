// The offline heap shimmers like starlight: a continuous field of twinkles, a fifth of the dots somewhere in one at any
// moment, each on a smooth ease-in-out envelope (rising over .7-1.3 s, fading over .9-1.7 s), out of step with each other,
// a slower glint that peaks at full white with a soft glow and a faint cross every few seconds; random (the same for the
// same seed), and only the twinkling dots' stripes change on the panel. cc via tools/test-render.py
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/face.h"
#include "../main/tess_internal.h"
#include "../main/canvas.h"

#define DT (1.f / 30)
#define SECONDS 240

static face_t offline(void) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    for (int i = 0; i < 4 * 30; i++) face_update(&f, DT);
    face_set_mode(&f, MODE_OFFLINE);
    for (int i = 0; i < 8 * 30; i++) face_update(&f, DT);  // the heap has come to rest
    return f;
}

static int lit_now(const face_t *f) {
    int n = 0;
    for (int k = 0; k < TESS_TWINKLES; k++) n += tess_twinkle_level(f, k) > 0;
    return n;
}

static float autocorrelation(const float *series, int n, int lag) {
    double mean = 0, var = 0, sum = 0;
    for (int i = 0; i < n; i++) mean += series[i] / n;
    for (int i = 0; i < n; i++) var += (series[i] - mean) * (series[i] - mean);
    for (int i = 0; i + lag < n; i++) sum += (series[i] - mean) * (series[i + lag] - mean);
    return (float)(sum / var);
}

// How many dots are in a twinkle: about a fifth, never a handful and never a crowd, and steady rather than pulsing.
static void the_field_is_dense_and_steady(void) {
    static float count[SECONDS * 30];
    face_t f = offline();
    int frames = SECONDS * 30, inside = 0, most = 0, least = TESS_N;
    double sum = 0;
    for (int i = 0; i < frames; i++) {
        face_update(&f, DT);
        int n = count[i] = (float)lit_now(&f);
        sum += n;
        inside += n >= TESS_N * 12 / 100 && n <= TESS_N * 25 / 100;
        most = most > n ? most : n;
        least = least < n ? least : n;
    }
    float share = (float)(sum / frames) * 100 / TESS_N, worst_echo = 0;
    for (int lag = 4 * 30; lag < 30 * 30; lag += 5) worst_echo = fmaxf(worst_echo, fabsf(autocorrelation(count, frames, lag)));
    printf("shimmer field: %.1f%% of the dots in a twinkle on average (%d to %d of %d; %d%% of the time within 12-25%%), echo of the count %.2f\n",
           share, least, most, TESS_N, 100 * inside / frames, worst_echo);
    assert(share > 15.f && share < 22.f);
    assert(inside > frames * 9 / 10);
    assert(least >= TESS_N * 10 / 100 && most <= TESS_N * 27 / 100);
    assert(worst_echo < .2f);  // nothing beats or pulses: once the twinkles of a moment are gone, no lag repeats the count
}

// Each twinkle's own lengths, one per frame at most, in random order; the brightness moves by small steps.
static void twinkles_are_slow_smooth_and_out_of_step(void) {
    face_t f = offline();
    float last_level[TESS_TWINKLES] = {0}, worst_step = 0, last_start = -1, gap_sum = 0, gap_sq = 0, min_gap = 9, max_gap = 0;
    float min_rise = 9, max_rise = 0, min_fade = 9, max_fade = 0, last_glint = -1, glint_min = 99, glint_max = 0, glint_fade = 0;
    int starts = 0, glints = 0, same_frame = 0;
    for (int k = 0; k < TESS_TWINKLES; k++) last_level[k] = tess_twinkle_level(&f, k);
    for (int i = 0; i < SECONDS * 30; i++) {
        face_update(&f, DT);
        float t = i * DT;
        int begun = 0;
        for (int k = 0; k < TESS_TWINKLES; k++) {
            float level = tess_twinkle_level(&f, k);
            bool fresh = f.tess_twinkle_age[k] == 0;
            if (fresh) {
                begun++;
                bool glint = f.tess_twinkle_peak[k] == 255;
                if (glint) {
                    if (last_glint >= 0) { glint_min = fminf(glint_min, t - last_glint); glint_max = fmaxf(glint_max, t - last_glint); }
                    last_glint = t;
                    glints++;
                    glint_fade = fmaxf(glint_fade, f.tess_twinkle_rise[k] + f.tess_twinkle_fade[k]);
                } else {
                    min_rise = fminf(min_rise, f.tess_twinkle_rise[k]); max_rise = fmaxf(max_rise, f.tess_twinkle_rise[k]);
                    min_fade = fminf(min_fade, f.tess_twinkle_fade[k]); max_fade = fmaxf(max_fade, f.tess_twinkle_fade[k]);
                }
                if (last_start >= 0) { float gap = t - last_start; gap_sum += gap; gap_sq += gap * gap; min_gap = fminf(min_gap, gap); max_gap = fmaxf(max_gap, gap); }
                last_start = t;
                starts++;
            } else {
                worst_step = fmaxf(worst_step, fabsf(level - last_level[k]));
            }
            last_level[k] = level;
        }
        same_frame += begun > 1;
    }
    float mean = gap_sum / (starts - 1), spread = sqrtf(gap_sq / (starts - 1) - mean * mean);
    printf("shimmer envelope: rise %.2f-%.2f s, fade %.2f-%.2f s, biggest brightness step %.3f a frame (of 1), a twinkle every %.2f s +-%.2f (%.2f-%.2f); %d glints, %.1f-%.1f s apart, %.1f s long\n",
           min_rise, max_rise, min_fade, max_fade, worst_step, mean, spread, min_gap, max_gap, glints, glint_min, glint_max, glint_fade);
    assert(min_rise >= .7f && max_rise <= 1.3f && min_fade >= .9f && max_fade <= 1.7f);
    assert(worst_step < .09f);                      // a twinkle never jumps: at most 9% of the range in a frame (a smoothstep's steepest)
    assert(spread > .2f * mean);                    // no fixed rhythm
    assert(same_frame == 0);                        // never two beginning together
    assert(glints > SECONDS / 7 && glints < SECONDS / 2);
    assert(glint_min >= 2.5f && glint_max <= 7.f);  // now and then, not on a beat
    assert(glint_fade > 3.f);                       // longer than any other
}

static void clear_twinkles(face_t *f) {
    for (int k = 0; k < TESS_TWINKLES; k++) f->tess_twinkle_age[k] = f->tess_twinkle_rise[k] = f->tess_twinkle_fade[k] = 0;
}

// A twinkle starts and ends slowly: no linear ramp, no hard on and off.
static void the_envelope_eases_in_and_out(void) {
    face_t f = offline();
    clear_twinkles(&f);
    f.tess_twinkle_rise[0] = 1.f; f.tess_twinkle_fade[0] = 1.5f; f.tess_twinkle_peak[0] = 255;
    float first = 0, quarter = 0, middle = 0, peak = 0;
    for (int i = 1; i <= 30; i++) {
        f.tess_twinkle_age[0] = i * DT;
        float level = tess_twinkle_level(&f, 0);
        if (i == 1) first = level;
        if (i == 8) quarter = level;
        if (i == 15) middle = level;
        if (i == 30) peak = level;
    }
    f.tess_twinkle_age[0] = 1.f + 1.5f - DT;
    float end = tess_twinkle_level(&f, 0);
    printf("shimmer easing: first frame %.4f, a quarter of the rise %.3f, half %.3f, peak %.3f, last frame %.4f\n", first, quarter, middle, peak, end);
    assert(first < .01f && end < .01f);        // eased in and out
    assert(quarter < .25f && middle > .4f && middle < .6f && peak > .99f);  // a smoothstep, not a ramp
}

static void same_seed_same_shimmer(void) {
    face_t a = offline(), b = offline();
    for (int i = 0; i < 60 * 30; i++) {
        face_update(&a, DT); face_update(&b, DT);
        assert(!memcmp(a.tess_twinkle_age, b.tess_twinkle_age, sizeof a.tess_twinkle_age));
        assert(!memcmp(a.tess_twinkle_point, b.tess_twinkle_point, sizeof a.tess_twinkle_point));
        assert(!memcmp(a.tess_twinkle_peak, b.tess_twinkle_peak, sizeof a.tess_twinkle_peak));
    }
}

// Every dot takes part, the ones lying still in the heap as much as any.
static void the_resting_dots_twinkle_too(void) {
    bool seen[TESS_N] = {false};
    face_t f = offline();
    int samples = 0, resting = 0, distinct = 0;
    for (int i = 0; i < 120 * 30; i++) {
        face_update(&f, DT);
        for (int k = 0; k < TESS_TWINKLES; k++) {
            if (tess_twinkle_level(&f, k) <= 0) continue;
            int p = f.tess_twinkle_point[k];
            samples++;
            resting += hypotf(f.tess_velocity[p][0], f.tess_velocity[p][1]) < .5f;
            distinct += !seen[p];
            seen[p] = true;
        }
    }
    printf("shimmer coverage: %d of %d dots twinkled in 2 min, %d%% of the twinkling ones lying still\n", distinct, TESS_N, 100 * resting / samples);
    assert(distinct >= TESS_N * 95 / 100);
    assert(resting > samples * 9 / 10);
}

static int grey_of(uint16_t color) { return (color >> 5 & 63) << 2; }

// The dot of the scene that sits where `at` does, or NULL unless exactly one does (piled exactly on another: not told apart).
static const render_dot_t *dot_in_place(const scene_t *scene, const render_dot_t *at) {
    const render_dot_t *found = NULL;
    int count = 0;
    for (int j = 0; j < scene->dots_n; j++) {
        if (scene->dots[j].x != at->x || scene->dots[j].y != at->y) continue;
        found = &scene->dots[j];
        count++;
    }
    return count == 1 ? found : NULL;
}

// A glint's star: four arms of three dots each.
#define STAR_DOTS 12

// A twinkle takes each dot from its low tone all the way to white and swells it; the dot stays grey (monochrome); a glint
// adds its star.
static void a_twinkle_lifts_every_tone_evenly(void) {
    face_t f = offline();
    scene_t rest, lit;
    clear_twinkles(&f);
    face_draw(&f, &rest);
    int lifted = 0, dimmest_peak = 255, dimmest_rest = 255, brightest_rest = 0;
    for (int i = 0; i < rest.dots_n; i++) {
        dimmest_rest = (int)fminf((float)dimmest_rest, (float)grey_of(rest.dots[i].color));
        brightest_rest = (int)fmaxf((float)brightest_rest, (float)grey_of(rest.dots[i].color));
    }
    for (int p = 0; p < TESS_N; p++) {
        clear_twinkles(&f);
        f.tess_twinkle_rise[0] = 1.f; f.tess_twinkle_fade[0] = 1.f; f.tess_twinkle_peak[0] = 255; f.tess_twinkle_point[0] = (uint8_t)p;
        f.tess_twinkle_age[0] = 1.f;  // the very top
        face_draw(&f, &lit);
        assert(lit.dots_n == rest.dots_n + STAR_DOTS);
        int changed = 0;
        for (int i = 0; i < STAR_DOTS; i++) assert(lit.dots[i].radius == 3 && lit.dots[i].color != 0);  // the star comes first
        for (int i = STAR_DOTS; i < lit.dots_n; i++) {
            const render_dot_t *b = &lit.dots[i], *a = dot_in_place(&rest, b);
            if (!a || (a->color == b->color && a->radius == b->radius)) continue;
            changed++;
            assert(b->radius > a->radius);
            uint16_t c = b->color;
            assert(abs((c >> 11) * 8 - (c >> 5 & 63) * 4) < 12 && abs((c & 31) * 8 - (c >> 5 & 63) * 4) < 12);  // grey
            assert(b->color != a->color);
            lifted++;
            dimmest_peak = (int)fminf((float)dimmest_peak, (float)grey_of(b->color));
        }
        assert(changed <= 1);
    }
    printf("shimmer tone: at rest %d..%d/255 grey, %d dots lifted to at least %d/255\n", dimmest_rest, brightest_rest, lifted, dimmest_peak);
    assert(lifted == TESS_N);
    assert(dimmest_rest >= 44 && brightest_rest <= 168);  // low, but above the crushed end of the panel: room to rise
    assert(dimmest_peak >= 252);                          // every dot peaks at full white
}

static void panel_px(const face_t *f, int i, float *x, float *y) {
    float p = 4.f / (4.7f - f->tess_position[i][2]);
    *x = 240 + 90 * f->tess_position[i][0] * p;
    *y = 255 + 90 * f->tess_position[i][1] * p;
}

static bool near_a_lit_dot(const face_t *f, int row, int stripe) {
    for (int k = 0; k < TESS_TWINKLES; k++) {
        float x, y;
        if (f->tess_twinkle_age[k] > f->tess_twinkle_rise[k] + f->tess_twinkle_fade[k] + DT) continue;  // (also just faded)
        panel_px(f, f->tess_twinkle_point[k], &x, &y);
        if (fabsf(y / 2 - row) < 16 && x / 2 > stripe * CANVAS_STRIPE - 16 && x / 2 < (stripe + 1) * CANVAS_STRIPE + 16) return true;
    }
    return false;
}

static void push_band(int x0, int y0, int x1, int y1, uint16_t *px, void *ctx) {
    (void)x0; (void)x1;
    assert(canvas_rows(ctx, y0, y1, px));
}

// Frame after frame, a dot that is not twinkling changes nothing on the panel; the twinkling ones repaint only their own
// stripes, a small part of the frame.
static void only_twinkling_dots_repaint(void) {
    static canvas_t diff;
    static render_state_t state;
    static uint16_t band[R_W * R_BAND];
    uint16_t *bufs[] = {band};
    face_t f = offline();
    for (int i = 0; i < 40 * 30; i++) face_update(&f, DT);  // the last of the heap creeps into place for a good half minute
    long stripes = 0, worst = 0;
    for (int i = 0; i < 90 * 30; i++) {
        face_update(&f, DT);
        scene_t scene;
        face_draw(&f, &scene);
        render_frame(&state, &scene, bufs, 1, false, push_band, &diff);
        if (i < 2) continue;  // the first frames are painted whole
        long changed = 0;
        for (int y = 0; y < CANVAS_H; y++) {
            for (int s = 0; s < CANVAS_STRIPES; s++) {
                if (!(diff.changed_mask[y] >> s & 1)) continue;
                changed++;
                assert(near_a_lit_dot(&f, y, s));
            }
        }
        stripes += changed;
        worst = worst > changed ? worst : changed;
    }
    float share = 100.f * stripes / (CANVAS_H * CANVAS_STRIPES) / (90 * 30 - 2);
    printf("shimmer cost: %.2f%% of the frame's stripes repainted on average, at most %.1f%% in one frame\n",
           share, 100.f * worst / (CANVAS_H * CANVAS_STRIPES));
    assert(share < 4.f && worst < CANVAS_H * CANVAS_STRIPES / 16);
}

// The heap settles at the bottom of the screen (down is where gravity pulls), inside its edges, in a pile a few dots
// deep rather than a single row.
static void the_heap_is_a_shallow_pile_inside_the_screen(void) {
    face_t f = offline();
    scene_t scene;
    float top = 999, bottom = 0, left = 999, right = 0, mean_y = 0;
    clear_twinkles(&f);
    face_draw(&f, &scene);
    for (int i = 0; i < scene.dots_n; i++) {  // the dots' edges, in panel pixels (their coordinates are eighths)
        float x = scene.dots[i].x / 8.f, y = scene.dots[i].y / 8.f, r = scene.dots[i].radius / 8.f;
        top = fminf(top, y - r); bottom = fmaxf(bottom, y + r); left = fminf(left, x - r); right = fmaxf(right, x + r);
        mean_y += y / scene.dots_n;
    }
    printf("shimmer pile: %.0f..%.0f px down, %.0f..%.0f px across, %.0f px deep on a 480 px panel\n", top, bottom, left, right, bottom - top);
    assert(bottom <= 480 && left >= 0 && right <= 480);  // in the screen
    assert(mean_y > 400);                             // at the bottom
    assert(bottom - top >= 24 && bottom - top < 90);   // deeper than one row, still a heap
}

// Visual glints remain alive at rest without a recurring audio background.
static void resting_glints_are_silent(void) {
    face_t f = offline();
    tess_cue_t cue;
    float strength, position;
    int glints = 0;
    for (int i = 0; i < SECONDS * 30; i++) {
        face_update(&f, DT);
        for (int k = 0; k < TESS_TWINKLES; k++) glints += f.tess_twinkle_age[k] == 0 && f.tess_twinkle_peak[k] == TESS_GLINT_PEAK;
        while (face_take_cue(&f, &cue, &strength, &position)) {
            assert(cue == TC_IMPACT && fabsf(position) <= 1.f);
            if (i > 30 * 10) assert(!"settled particles must be silent");
        }
    }
    assert(glints > 0);
}

int main(void) {
    the_field_is_dense_and_steady();
    twinkles_are_slow_smooth_and_out_of_step();
    the_envelope_eases_in_and_out();
    same_seed_same_shimmer();
    the_resting_dots_twinkle_too();
    a_twinkle_lifts_every_tone_evenly();
    only_twinkling_dots_repaint();
    the_heap_is_a_shallow_pile_inside_the_screen();
    resting_glints_are_silent();
    puts("tess shimmer ok");
    return 0;
}
