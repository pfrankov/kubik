#include "tess_sky_internal.h"
#include <math.h>
#include <string.h>

// Chart geometry and playable routes are checked offline with this same 112-particle stream.
// Preparing a sky is now bounded table work, without searching/probing beside the ending melody.
float sky_degree_hz(int degree) {
    static const uint8_t semitones[5] = {0, 2, 4, 7, 9};
    if (degree < 0) degree = 0;
    return 293.665f * powf(2.f, (12.f * (float)(degree / 5) + semitones[degree % 5]) / 12.f);
}

void sky_compose(sky_world_t *w, uint32_t seed, const sky_recipe_t *recipe) {
    memset(w, 0, sizeof *w);
    w->rng = seed ? seed : 1u;
    w->feed_scale = 1;
    w->root = recipe->root % 5;
    sky_place_chart(w, recipe);
    static const uint8_t ladder[SKY_STARS] = {0, 2, 4, 5, 7, 9, 11};
    for (int i = 0; i < w->count_stars; i++) {
        sky_star_t *star = &w->star[i];
        int degree = w->root + ladder[i];
        while (degree > 9) degree -= 5;
        star->degree = (uint8_t)degree;
        star->hz = sky_degree_hz(degree);
        star->color = (uint8_t)((i + w->root) % COL_COUNT);
    }
}
