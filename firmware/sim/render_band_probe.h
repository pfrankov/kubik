// Host reference of the render regression test. Header only.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../main/render.h"

// The reference for what a band sends (present.c plans several windows per band): one panel window for pairs p0..p1-1
// of panel rows. Panel rows come in pairs, one per native row: a pair shows its native row and, on the odd line, the
// midpoint with the next one; every panel column past the first also blends in the native pixel to its left. Trimmed
// to the pairs that changed.
static inline bool render_band_window(const uint8_t *span0, const uint8_t *span1, int p0, int p1, render_win_t *w) {
    int lo = R_W, hi = 0, first = -1, last = -1;
    for (int p = p0; p < p1; p++) {
        int a = R_W, b = 0;
        for (int y = p; y <= p + 1 && y < R_H; y++)
            if (span0[y] < span1[y]) {
                if (span0[y] < a) a = span0[y];
                if (span1[y] > b) b = span1[y];
            }
        if (a >= b) continue;
        if (first < 0) first = p;
        last = p;
        if (a < lo) lo = a;
        if (b > hi) hi = b;
    }
    if (first < 0) return false;
    *w = (render_win_t){(int16_t)((lo > 0 ? lo - 1 : 0) * R_SCALE), (int16_t)(first * R_SCALE), (int16_t)(hi * R_SCALE),
                        (int16_t)((last + 1) * R_SCALE)};
    return true;
}
