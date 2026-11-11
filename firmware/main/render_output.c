#include "render.h"

#include <string.h>

typedef uint32_t pixel_pair_t __attribute__((__may_alias__));

static int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

static int band_windows(const uint16_t *mask, int p0, int p1, render_win_t *out) {
    uint32_t pair[R_BAND * 2], all = 0;
    for (int p = p0; p < p1; p++) {
        pair[p - p0] = mask[p] | (p + 1 < R_H ? mask[p + 1] : 0);
        all |= pair[p - p0];
    }
    all |= (all << 1) & (all >> 1);
    // At 40 MHz two unchanged stripes cost less than another panel window's
    // commands and DMA setup. They are still rendered and sent losslessly.
    all |= ((all << 1) & (all >> 2)) | ((all << 2) & (all >> 1));
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

static int window_cost(const render_win_t *windows, int n) {
    // Measured C6 queue/setup cost expressed as equivalent QSPI panel pixels.
    int cost = n * 1000;
    for (int i = 0; i < n; i++)
        cost += (windows[i].x1 - windows[i].x0) * (windows[i].y1 - windows[i].y0);
    return cost;
}

int render_band_windows(const uint16_t *mask, int p0, int p1, render_win_t *out) {
    int n = band_windows(mask, p0, p1, out);
    if (p1 - p0 < 2) return n;
    render_win_t split[RENDER_BAND_WINDOWS * 2];
    int mid = (p0 + p1) / 2;
    int first = band_windows(mask, p0, mid, split);
    int halves = first + band_windows(mask, mid, p1, split + first);
    if (halves <= RENDER_BAND_WINDOWS && window_cost(split, halves) < window_cost(out, n)) {
        memcpy(out, split, (size_t)halves * sizeof *out);
        return halves;
    }
    return n;
}

void render_output_begin(render_output_t *st, render_row_fn read, void *ctx) {
    st->read = read; st->ctx = ctx; st->row_lit = NULL;
    st->key[0] = st->key[1] = -1;
    st->xa = 0; st->xb = R_W;
}

void render_output_black_rows(render_output_t *st, const uint16_t *row_lit) {
    st->row_lit = row_lit;
    st->key[0] = st->key[1] = -1;
}

void render_output_columns(render_output_t *st, int x0, int x1) {
    int xa = imax(x0 / R_SCALE, 0), xb = imin((x1 + R_SCALE - 1) / R_SCALE, R_W);
    if (xa < st->xa || xb > st->xb)  // cached rows may lack these columns (a reader may fill only the asked ones)
        st->key[0] = st->key[1] = -1;
    st->xa = xa; st->xb = xb;
}

static inline uint32_t midpoint2(uint32_t a, uint32_t b) {
    return (a & b) + (((a ^ b) & 0xF7DEF7DEu) >> 1);
}
static inline uint32_t swap2(uint32_t v) {
    return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
}
// UI backgrounds need no interpolation arithmetic. Keep the exact same pairs.
static R_HOT bool flat_row(render_output_t *st, int k, const uint16_t *src) {
    int lo = st->xa & ~1, hi = imin(st->xb + 2, R_W);
    uint16_t color = src[lo];
    for (int x = lo + 1; x < hi; x++) if (src[x] != color) return false;
    uint32_t pair = color | ((uint32_t)color << 16);
    if (!color) {
        size_t bytes = (size_t)(st->xb - lo) * sizeof st->row[k][0];
        memset(st->row[k] + lo, 0, bytes);
    } else for (int x = lo; x < st->xb; x++) { st->row[k][x] = pair; }
    return true;
}

// Native row y as expanded pairs, for columns xa..xb-1 (rounded out to whole pixel pairs).
static R_HOT int output_row(render_output_t *st, int y) {
    int k = y & 1;
    if (st->key[k] != y && st->row_lit && !st->row_lit[y]) {
        int lo = st->xa & ~1;
        memset(st->row[k] + lo, 0, (size_t)(st->xb - lo) * sizeof st->row[k][0]);
        st->flat[k] = true;
        st->key[k] = y;
    }
    if (st->key[k] != y) {
        _Alignas(4) uint16_t src[R_W + 1];
        int last = imin(st->xb + 2, R_W);
        st->read(st->ctx, y, src, st->xa & ~1, last);  // pairs and their right neighbour
        if (last == R_W) src[R_W] = src[R_W - 1];
        uint32_t *row = st->row[k];
        st->flat[k] = flat_row(st, k, src);
        // Two pixels per step: (a0, a1) and their right neighbours (a1, a2) give both midpoints at once.
#pragma GCC unroll 2
        for (int x = st->xa & ~1; !st->flat[k] && x < st->xb; x += 2) {
            uint32_t w = *(const pixel_pair_t *)(const void *)(src + x);
            // Sparse clouds leave long black spans inside otherwise changed rows.
            // Three zero source pixels expand to four exact zero panel pixels.
            if (!w && !src[x + 2]) {
                row[x] = row[x + 1] = 0;
                continue;
            }
            uint32_t m = midpoint2(w, (w >> 16) | ((uint32_t)src[x + 2] << 16));
            uint32_t r0 = (w & 0xFFFFu) | (m << 16), r1 = (w >> 16) | (m & 0xFFFF0000u);
            row[x] = r0;
            row[x + 1] = r1;
        }
        st->key[k] = y;
    }
    return k;
}

static inline void fill_pair_row(pixel_pair_t *out, uint32_t color, int pairs) {
    if (!color) memset(out, 0, (size_t)pairs * sizeof *out);
    else for (int x = 0; x < pairs; x++) out[x] = color;
}

R_HOT void render_expand_2x(render_output_t *st, uint16_t *out, int x0, int y0, int width, int rows, int stride, bool swap) {
    int n = width / R_SCALE, c = x0 / R_SCALE;
    for (int y = y0; y < y0 + rows; y += R_SCALE) {
        int sy = y / R_SCALE;
        int ka = output_row(st, sy), kb = output_row(st, imin(sy + 1, R_H - 1));
        const uint32_t *a = st->row[ka] + c, *b = st->row[kb] + c;
        pixel_pair_t *same = (pixel_pair_t *)out, *odd = (pixel_pair_t *)(out + stride);  // even panel row: the native row itself
        if (st->flat[ka] && st->flat[kb]) {
            uint32_t color = a[0], middle = midpoint2(a[0], b[0]);
            fill_pair_row(same, swap ? swap2(color) : color, n);
            fill_pair_row(odd, swap ? swap2(middle) : middle, n);
        } else if (swap) {
#pragma GCC unroll 4
            for (int x = 0; x < n; x++) {
                uint32_t even = swap2(a[x]);
                same[x] = even;
                odd[x] = a[x] == b[x] ? even : swap2(midpoint2(a[x], b[x]));
            }
        } else {
            for (int x = 0; x < n; x++) {
                same[x] = a[x];
                odd[x] = a[x] == b[x] ? a[x] : midpoint2(a[x], b[x]);
            }
        }
        out += stride * R_SCALE;
    }
}
