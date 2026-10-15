#include "render_internal.h"

#include <string.h>

static inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

// Per frame: which dots reach each band (bit i = dot i, so a band still draws them in scene order),
// and each glowing dot's halo reciprocal, so the pixels need no division.
#define DOT_WORDS ((R_MAX_DOTS + 31) / 32)
static uint32_t s_band_dots[R_BANDS][DOT_WORDS];
static int s_paint_radius2[R_MAX_DOTS];
static uint16_t s_paint_radius[R_MAX_DOTS];
static uint32_t s_halo_step[R_MAX_DOTS];  // floor(18 * 2^32 / outer^2) + 1: exact while outer < 256 (Q4)

// A halo's outer pixels round to zero alpha. Find its exact first nonzero
// coverage once per dot, so those pixels never enter the expensive blend path.
static int paint_radius2(int radius, int outer, unsigned glow, uint32_t step) {
    int core = (radius + 8) * (radius + 8);
    if (!step) return core;
    unsigned level = 1;
    while (level <= 18 && ((level * level / 18) * (glow + 1) >> 8) == 0) ++level;
    if (level > 18) return core;
    uint64_t coverage = (((uint64_t)level << 32) + step - 1) / step;
    int limit = (uint64_t)(outer * outer) >= coverage ? outer * outer - (int)coverage + 1 : 0;
    return imax(core, limit);
}

// Integer floor square root, once per dot: exact coverage bounds, not a
// smaller visual radius. Every pixel omitted has alpha zero in the oracle.
static unsigned coverage_radius(unsigned n) {
    unsigned root = 0, bit = 1u << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) { n -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return root;
}

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
        s_paint_radius2[i] = paint_radius2(r, outer, p->glow, s_halo_step[i]);
        int extent = (int)coverage_radius((unsigned)(s_paint_radius2[i] - 1));
        s_paint_radius[i] = (uint16_t)extent;
        int first = imax(0, ((p->y - extent) >> 4) / R_BAND), last = imin(R_BANDS - 1, ((p->y + extent) >> 4) / R_BAND);
        for (int band = first; band <= last; band++) s_band_dots[band][i >> 5] |= 1u << (i & 31);
    }
}

typedef struct {
    uint32_t foreground;
    uint16_t opaque, black[33];
} dot_blend_t;

// Sparse dots mostly cover black. Prepare every exact alpha result once,
// including colors that round to zero; overlaps keep the general blend.
static inline R_HOT uint16_t blend_dot(dot_blend_t *cache, uint16_t background, unsigned alpha, bool swap) {
    if (alpha == 32) return cache->opaque;
    if (!background) return cache->black[alpha];
    uint16_t canonical = swap ? rgb565_bswap(background) : background;
    uint32_t packed = (canonical | ((uint32_t)canonical << 16)) & 0x07E0F81F;
    packed = (packed + (((cache->foreground - packed) * alpha) >> 5)) & 0x07E0F81F;
    uint16_t color = (uint16_t)(packed | (packed >> 16));
    if (swap) color = rgb565_bswap(color);
    return color;
}

static inline void prepare_blend(dot_blend_t *cache, uint16_t color, bool swap) {
    cache->foreground = (color | ((uint32_t)color << 16)) & 0x07E0F81F;
    cache->opaque = swap ? rgb565_bswap(color) : color;
    for (unsigned alpha = 0; alpha < 32; alpha++) {
        uint32_t packed = ((cache->foreground * alpha) >> 5) & 0x07E0F81F;
        uint16_t blended = (uint16_t)(packed | (packed >> 16));
        cache->black[alpha] = swap ? rgb565_bswap(blended) : blended;
    }
}

// floor(h*h/18), h=0..18. The reciprocal maps covered halo pixels into
// this closed range; retain the scalar rounding without per-pixel division.
static const uint8_t s_halo_curve[19] = {0, 0, 0, 0, 0, 1, 2, 2, 3, 4, 5, 6, 8, 9, 10, 12, 14, 16, 18};

// Reject the two transparent row ends once. The remaining circular span has
// positive coverage, including subpixel centres and clipped native windows.
static inline void dot_span(int cx, int dy2, int paint_rr, int *first, int *last) {
    while (*first <= *last) {
        int dx = *first * 16 + 8 - cx;
        if (dx * dx + dy2 < paint_rr) break;
        ++*first;
    }
    while (*last >= *first) {
        int dx = *last * 16 + 8 - cx;
        if (dx * dx + dy2 < paint_rr) break;
        --*last;
    }
}

// Small antialiased points: squared-distance coverage, no per-pixel float, square root or division.
static inline R_HOT void raster_dot(const render_dot_t *p, uint32_t halo_step, int paint_rr, int extent, uint16_t *buf,
                                   int x0, int x1, int y0, int y1, bool swap) {
    int width = x1 - x0 + 1, cx = p->x, cy = p->y;
    unsigned glow = p->glow;
    dot_blend_t cache;
    prepare_blend(&cache, p->color, swap);
    int r = p->radius, core = r + 8, outer = p->glow ? r * 3 + 16 : core;
    int ya = imax(y0, (cy - extent) >> 4), yb = imin(y1, (cy + extent) >> 4);
    if (ya > yb) return;
    int xa = imax(x0, (cx - extent) >> 4), xb = imin(x1, (cx + extent) >> 4);
    int rr = outer * outer, cr = core * core, core_full = r * 32;
    // floor(inside / r) as a multiply: exact for inside < 32 r while r < 1448 (Q4).
    uint32_t core_step = (1u << 26) / (uint32_t)imax(1, r) + 1;
    for (int y = ya; y <= yb; y++) {
        int dy = y * 16 + 8 - cy;
        int dy2 = dy * dy;
        if (dy2 >= paint_rr) continue;
        uint16_t *row = buf + (y - y0) * width;
        int first = xa, last = xb;
        dot_span(cx, dy2, paint_rr, &first, &last);
        int dx = first * 16 + 8 - cx, distance = dx * dx + dy2, delta = dx * 32 + 256;
        // (dx+16)^2-dx^2 = 32*dx+256; no square or coverage branch per pixel.
        for (int x = first; x <= last; x++, distance += delta, delta += 512) {
            int inside = cr - distance, a = 32;
            // An opaque core already dominates the halo: no halo arithmetic or
            // blend is needed there. The antialiased edge keeps both coverages.
            if (inside <= 0 || inside < core_full) {
                a = 0;
                if (halo_step) {
                    unsigned halo = (unsigned)(((uint64_t)(uint32_t)(rr - distance) * halo_step) >> 32);
                    a = (s_halo_curve[halo] * (glow + 1)) >> 8;
                }
                if (inside > 0) a = imax(a, (int)(((uint32_t)inside * core_step) >> 26));
            }
            if (!a) continue;
            row[x - x0] = blend_dot(&cache, row[x - x0], (unsigned)a, swap);
        }
    }
}

R_HOT void render_raster_dots(const scene_t *s, uint16_t *buf, int x0, int x1, int y0, int y1) {
    const uint32_t *band = s_band_dots[y0 / R_BAND];
    for (int i = 0; i < DOT_WORDS * 32; i++) {
        uint32_t bits = band[i >> 5] >> (i & 31);  // this band's dots from i on, in this word
        if (!bits) { i |= 31; continue; }
        i += __builtin_ctz(bits);
        raster_dot(&s->dots[i], s_halo_step[i], s_paint_radius2[i], s_paint_radius[i], buf, x0, x1, y0, y1, g_render_swap);
    }
}
