#include "render.h"

#include <string.h>

typedef uint32_t pixel_pair_t __attribute__((__may_alias__));

static int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

int render_band_windows(const uint16_t *mask, int p0, int p1, render_win_t *out) {
    uint32_t pair[R_BAND * 2], all = 0;
    for (int p = p0; p < p1; p++) {
        pair[p - p0] = mask[p] | (p + 1 < R_H ? mask[p + 1] : 0);
        all |= pair[p - p0];
    }
    all |= (all << 1) & (all >> 1);  // one unchanged stripe between two changed ones costs less to send than a window
    int n = 0;
    for (int k = 0; k < R_W / 16 && n < RENDER_BAND_WINDOWS;) {
        if (!(all >> k & 1)) { k++; continue; }
        int end = k;
        while (end < R_W / 16 && (all >> end & 1)) end++;
        uint32_t run = ((1u << end) - 1) & ~((1u << k) - 1);
        int first = -1, last = -1;
        for (int p = p0; p < p1; p++)
            if (pair[p - p0] & run) {
                if (first < 0) first = p;
                last = p;
            }
        if (first >= 0)
            out[n++] = (render_win_t){(int16_t)((k > 0 ? k * 16 - 1 : 0) * R_SCALE), (int16_t)(first * R_SCALE),
                                      (int16_t)(end * 16 * R_SCALE), (int16_t)((last + 1) * R_SCALE)};
        k = end;
    }
    return n;
}

void render_output_begin(render_output_t *st, render_row_fn read, void *ctx) {
    st->read = read; st->ctx = ctx;
    st->key[0] = st->key[1] = st->source_key[0] = st->source_key[1] = -1;
    st->xa = 0; st->xb = R_W;
}

void render_output_columns(render_output_t *st, int x0, int x1) {
    int xa = imax(x0 / R_SCALE, 0), xb = imin((x1 + R_SCALE - 1) / R_SCALE, R_W);
    if (xa < st->xa || xb > st->xb)  // cached rows may lack these columns (a reader may fill only the asked ones)
        st->key[0] = st->key[1] = st->source_key[0] = st->source_key[1] = -1;
    st->xa = xa; st->xb = xb;
}

static inline uint32_t midpoint2(uint32_t a, uint32_t b) {
    return (a & b) + (((a ^ b) & 0xF7DEF7DEu) >> 1);
}
static inline uint32_t swap2(uint32_t v) {
    return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
}
// Native row y as expanded pairs, for columns xa..xb-1 (rounded out to whole pixel pairs).
static R_HOT int output_row(render_output_t *st, int y, bool swap) {
    int k = y & 1;
    if (st->key[k] != y || st->swap[k] != swap) {
        uint16_t *src = st->source[k];
        if (st->source_key[k] != y) {
            st->read(st->ctx, y, src, st->xa & ~1, imin(st->xb + 2, R_W));  // the pairs and the neighbour of the last one
            src[R_W] = src[R_W - 1];  // the last pixel's "next" is itself
            st->source_key[k] = y;
        }
        uint32_t *row = st->row[k], *even = st->even[k];
        // Two pixels per step: (a0, a1) and their right neighbours (a1, a2) give both midpoints at once.
#pragma GCC unroll 2
        for (int x = st->xa & ~1; x < st->xb; x += 2) {
            uint32_t w = *(const pixel_pair_t *)(const void *)(src + x);
            uint32_t m = midpoint2(w, (w >> 16) | ((uint32_t)src[x + 2] << 16));
            uint32_t r0 = (w & 0xFFFFu) | (m << 16), r1 = (w >> 16) | (m & 0xFFFF0000u);
            row[x] = r0;
            row[x + 1] = r1;
            even[x] = swap ? swap2(r0) : r0;
            even[x + 1] = swap ? swap2(r1) : r1;
        }
        st->key[k] = y;
        st->swap[k] = swap;
    }
    return k;
}

R_HOT void render_expand_2x(render_output_t *st, uint16_t *out, int x0, int y0, int width, int rows, bool swap) {
    int n = width / R_SCALE, c = x0 / R_SCALE;
    for (int y = y0; y < y0 + rows; y += R_SCALE) {
        int sy = y / R_SCALE;
        int ka = output_row(st, sy, swap), kb = output_row(st, imin(sy + 1, R_H - 1), swap);
        const uint32_t *a = st->row[ka] + c, *b = st->row[kb] + c, *even = st->even[ka] + c;
        pixel_pair_t *same = (pixel_pair_t *)out, *odd = (pixel_pair_t *)(out + width);  // even panel row: the native row itself
        if (swap) {
#pragma GCC unroll 4
            for (int x = 0; x < n; x++) {
                same[x] = even[x];
                odd[x] = swap2(midpoint2(a[x], b[x]));
            }
        } else {
            for (int x = 0; x < n; x++) {
                same[x] = even[x];
                odd[x] = midpoint2(a[x], b[x]);
            }
        }
        out += width * R_SCALE;
    }
}
