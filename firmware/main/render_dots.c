#include "render_internal.h"

#include <string.h>

static inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

// Per frame: which dots reach each band (bit i = dot i, so a band still draws them in scene order),
// and each glowing dot's halo reciprocal, so the pixels need no division.
#define DOT_WORDS ((R_MAX_DOTS + 31) / 32)
static uint32_t s_band_dots[R_BANDS][DOT_WORDS];
static uint32_t s_halo_step[R_MAX_DOTS];  // floor(18 * 2^32 / outer^2) + 1: exact while outer < 256 (Q4)

void render_dots_prepare(const scene_t *s) {
    memset(s_band_dots, 0, sizeof s_band_dots);
    for (int i = 0; i < s->dots_n; i++) {
        const render_dot_t *p = &s->dots[i];
        int r = p->radius, outer = p->glow ? r * 3 + 16 : r + 8;
        s_halo_step[i] = 0;
        if (p->glow) {  // 2^32 = whole rr + part, in 32-bit divisions (the 64-bit one is a slow library call)
            uint32_t rr = (uint32_t)(outer * outer), whole = UINT32_MAX / rr, part = UINT32_MAX - whole * rr + 1;
            s_halo_step[i] = 18 * whole + 18 * part / rr + 1;
        }
        int first = imax(0, ((p->y - outer) >> 4) / R_BAND), last = imin(R_BANDS - 1, ((p->y + outer) >> 4) / R_BAND);
        for (int band = first; band <= last; band++) s_band_dots[band][i >> 5] |= 1u << (i & 31);
    }
}

// Small antialiased points: squared-distance coverage, no per-pixel float, square root or division.
static inline R_HOT void raster_dot(const render_dot_t *p, uint32_t halo_step, uint16_t *buf,
                                   int x0, int x1, int y0, int y1, bool swap) {
    int width = x1 - x0 + 1;
    int r = p->radius, core = r + 8, outer = p->glow ? r * 3 + 16 : core;
    int ya = imax(y0, (p->y - outer) >> 4), yb = imin(y1, (p->y + outer) >> 4);
    if (ya > yb) return;
    int xa = imax(x0, (p->x - outer) >> 4), xb = imin(x1, (p->x + outer) >> 4);
    int rr = outer * outer, cr = core * core, core_full = r * 32;
    // floor(inside / r) as a multiply: exact for inside < 32 r while r < 1448 (Q4).
    uint32_t core_step = (1u << 26) / (uint32_t)imax(1, r) + 1;
    for (int y = ya; y <= yb; y++) {
        int dy = y * 16 + 8 - p->y;
        int dy2 = dy * dy;
        if (dy2 >= rr) continue;
        uint16_t *row = buf + (y - y0) * width;
        for (int x = xa; x <= xb; x++) {
            int dx = x * 16 + 8 - p->x;
            int distance = dx * dx + dy2;
            int coverage = rr - distance;
            if (coverage <= 0) continue;
            // Bounded soft halo (coverage * 18 / rr), plus an opaque antialiased core. Integer-only.
            int halo = (int)(((uint64_t)(uint32_t)coverage * halo_step) >> 32);
            halo = ((halo * halo / 18) * (p->glow + 1)) >> 8;
            int inside = cr - distance, a = halo;
            if (inside > 0) a = imax(a, inside >= core_full ? 32 : (int)(((uint32_t)inside * core_step) >> 26));
            if (!a) continue;
            uint16_t background = row[x - x0];
            if (swap) background = rgb565_bswap(background);
            uint16_t color = rgb565_blend(p->color, background, (unsigned)a);
            row[x - x0] = swap ? rgb565_bswap(color) : color;
        }
    }
}

R_HOT void render_raster_dots(const scene_t *s, uint16_t *buf, int x0, int x1, int y0, int y1) {
    const uint32_t *band = s_band_dots[y0 / R_BAND];
    for (int i = 0; i < DOT_WORDS * 32; i++) {
        uint32_t bits = band[i >> 5] >> (i & 31);  // this band's dots from i on, in this word
        if (!bits) { i |= 31; continue; }
        i += __builtin_ctz(bits);
        raster_dot(&s->dots[i], s_halo_step[i], buf, x0, x1, y0, y1, g_render_swap);
    }
}
