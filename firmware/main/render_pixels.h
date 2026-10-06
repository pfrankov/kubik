// Writing raster output into a band row: partial coverage, solid runs, translucent runs. Shared by the shape
// rasteriser; every function is inline because the loops that call them are the hot path.
#pragma once

#include "render_internal.h"

// Blends native colour fg over a pixel stored in the output byte order.
static inline uint16_t blend_out(uint16_t fg, uint16_t dst, uint32_t a) {
    if (!g_render_swap) return rgb565_blend(fg, dst, a);
    return rgb565_bswap(rgb565_blend(fg, rgb565_bswap(dst), a));
}
// col: p->color in the output byte order.
static inline void put_px(const prim_t *p, uint16_t col, uint16_t *row, int x, int32_t d, int32_t half, int32_t inv,
                          uint32_t solid_a) {
    if (d >= half) return;
    if (d <= -half) {
        row[x] = p->alpha == 255 ? col : blend_out(p->color, row[x], solid_a);
        return;
    }
    int32_t cov = 128 - ((d * inv) >> 16);
    if (cov > 256) cov = 256;
    if (cov > 0) {
        uint32_t a = ((uint32_t)cov * ((uint32_t)p->alpha + 1)) >> 11;
        row[x] = a >= 32 ? col : blend_out(p->color, row[x], a);
    }
}
// Runs are written a pixel pair at a time once they are 4-byte aligned.
static inline void fill_solid(uint16_t *q, uint16_t *stop, uint16_t col) {
    if (q < stop && ((uintptr_t)q & 2)) *q++ = col;
    for (; q + 1 < stop; q += 2) *pair_at(q) = pair_of(col);
    if (q < stop) *q = col;
}

// A translucent run mostly covers one colour (the background): the blend of the last new colour is reused.
static inline uint16_t blend_reuse(const prim_t *p, uint16_t below, uint32_t a, uint16_t *seen, uint16_t *out) {
    if (below != *seen) *seen = below, *out = blend_out(p->color, below, a);
    return *out;
}
static inline void fill_blend(const prim_t *p, uint16_t *q, uint16_t *stop, uint32_t a) {
    if (q >= stop) return;
    uint16_t seen = *q, out = blend_out(p->color, seen, a);
    if ((uintptr_t)q & 2) *q++ = out;
    for (; q + 1 < stop; q += 2) {
        if (*pair_at(q) == pair_of(seen)) {
            *pair_at(q) = pair_of(out);
        } else {
            q[0] = blend_reuse(p, q[0], a, &seen, &out);
            q[1] = blend_reuse(p, q[1], a, &seen, &out);
        }
    }
    if (q < stop) *q = blend_reuse(p, *q, a, &seen, &out);
}
static inline void fill_run(const prim_t *p, uint16_t col, uint16_t *row, int x, int end, uint32_t solid_a) {
    if (p->alpha == 255) fill_solid(row + x, row + end, col);
    else fill_blend(p, row + x, row + end, solid_a);
}
