#include "tess_internal.h"

// The same 16 vertex slots open into a square, cube, and tesseract. Scaling
// axes before the existing edge sampler keeps each form topologically tied to
// the next; no new point lattice or per-frame allocation is needed.

void tess_growth_vertices(const face_t *f, float vertices[16][4]) {
    const tess_games_t *games = &f->tess_games;
    // Old fixtures and the fully grown, un-folded Tess take the exact original
    // function: all existing adult projection references stay bit-for-bit.
    if (!games->ready || !isfinite(games->form) || !isfinite(games->fold)) {
        tess_vertices4d(f, vertices);
        return;
    }
    if (games->form >= 3.f && games->fold == 0.f) {
        tess_vertices4d(f, vertices);
        return;
    }

    float form = clampf(games->form, 0.f, 3.f);
    float fold = clampf(games->fold, 0.f, 1.f);
    float axis[4] = {clampf(form, 0.f, 1.f), clampf(form, 0.f, 1.f),
                     clampf(form - 1.f, 0.f, 1.f), clampf(form - 2.f, 0.f, 1.f)};
    float scale = 1.f - fold;
    tess_turn4d_t turn;
    tess_turn4d_prepare_scaled(f, axis[3], &turn);
    for (int i = 0; i < 16; i++) {
        float v[4] = {
            (i & 1 ? 1.f : -1.f) * axis[0],
            (i & 2 ? 1.f : -1.f) * axis[1],
            (i & 4 ? 1.f : -1.f) * axis[2],
            (i & 8 ? 1.f : -1.f) * axis[3],
        };
        tess_turn4d_apply(&turn, v);
        for (int a = 0; a < 4; a++)
            vertices[i][a] = isfinite(v[a]) ? v[a] * scale : 0.f;
    }
}
