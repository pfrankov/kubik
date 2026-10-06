#include "render_internal.h"
#include "render_pixels.h"

#include <math.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif
uint32_t image_us, shapes_us, glass_us;
#ifdef ESP_PLATFORM
#define R_NOW() ((uint32_t)esp_timer_get_time())
#else
#define R_NOW() 0u
#endif

#define Q 16  // sub-pixel units per pixel

static inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }
static inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

// floor(sqrt(n)): table seed (sqrt(i) in Q11 for i < 1024, interpolated) plus an exact
// one-step correction. ~4x cheaper than the bitwise loop on this FPU-less core.
static uint16_t s_sqrt_t[1025];
static void isqrt_init(void) {
    for (int i = 0; i <= 1024; i++) {
        uint32_t v = (uint32_t)(sqrtf((float)i) * 2048.f + 0.5f);
        s_sqrt_t[i] = (uint16_t)(v > 65535 ? 65535 : v);
    }
}
static inline uint32_t isqrt(uint32_t n) {
    if (n < 2) return n;
    int s = 0;
    if (n >= 1024) s = (32 - __builtin_clz(n) - 10 + 1) & ~1;  // idx = n >> s in [256, 1024)
    uint32_t idx = n >> s, frac = n & ((1u << s) - 1);
    uint32_t t = s_sqrt_t[idx] + ((((uint32_t)s_sqrt_t[idx + 1] - s_sqrt_t[idx]) * frac) >> s);
    uint32_t r = (t << (s >> 1)) >> 11;
    if (r * r > n) r--;
    else if (r < 65535 && (r + 1) * (r + 1) <= n) r++;
    return r;
}

static inline int32_t len2(int32_t x, int32_t y) { return (int32_t)isqrt((uint32_t)x * (uint32_t)x + (uint32_t)y * (uint32_t)y); }

// ------------------------------------------------------------------ SDF

static int32_t sdf(const scene_t *s, const prim_t *p, int32_t wx, int32_t wy);
static bool s_noclip[R_MAX_PRIMS];  // clip shape covers the whole bbox: skip it per pixel
static bool s_plain[R_MAX_PRIMS];   // upright rounded box, no modifiers, nothing to clip: the distance is closed form

static inline int32_t sdf_modifiers(const prim_t *p, int32_t x, int32_t y, int32_t d) {
    for (int i = 0; i < p->ncut; i++) {
        int32_t dc = ((p->cut_nx[i] * x + p->cut_ny[i] * y) >> 14) - p->cut_c[i];
        d = imax(d, dc);
    }
    if (p->has_sub) {
        int32_t ds = len2(x - p->sub_x, y - p->sub_y) - p->sub_r;
        d = imax(d, -ds);
    }
    if (p->stroke) {
        int32_t h = p->stroke >> 1;
        d = iabs(d + h) - h;
    }
    return d;
}
static inline int32_t sdf_rbox(const prim_t *p, int32_t x, int32_t y) {
    int32_t qx = iabs(x) - (p->a - p->r);
    int32_t qy = iabs(y) - (p->b - p->r);
    return (qx > 0 && qy > 0 ? len2(qx, qy) : imax(qx, qy)) - p->r;
}
static R_HOT int32_t sdf_local(const prim_t *p, int32_t x, int32_t y) {
    int32_t d;
    switch (p->kind) {
    case PK_RBOX:
        d = sdf_rbox(p, x, y);
        break;
    case PK_RING:
        d = iabs(len2(x, y) - p->a) - p->r;
        break;
    case PK_ARC: {
        int32_t ax = iabs(x);
        if ((int64_t)p->ap_c * ax > (int64_t)p->ap_s * y) {
            int32_t ex = (p->ap_s * p->a) >> 14, ey = (p->ap_c * p->a) >> 14;
            d = len2(ax - ex, y - ey) - p->r;
        } else {
            d = iabs(len2(x, y) - p->a) - p->r;
        }
        break;
    }
    case PK_HEART: {
        // Inigo Quilez's exact heart; unit K spans the lower tip to the lobe centres.
        int32_t K = p->a;  // p->b, p->r: the constants derived from K (canvas_geometry)
        int32_t X = iabs(x);
        int32_t Y = -y + p->b;
        if (Y + X > K) {
            d = len2(X - (K >> 2), Y - ((K * 3) >> 2)) - p->r;
        } else {
            int32_t d1x = X, d1y = Y - K;
            int32_t t = imax(X + Y, 0) / 2;
            int32_t d2x = X - t, d2y = Y - t;
            uint32_t m1 = (uint32_t)d1x * (uint32_t)d1x + (uint32_t)d1y * (uint32_t)d1y;
            uint32_t m2 = (uint32_t)d2x * (uint32_t)d2x + (uint32_t)d2y * (uint32_t)d2y;
            int32_t dd = (int32_t)isqrt(m1 < m2 ? m1 : m2);
            d = (X - Y) < 0 ? -dd : dd;
        }
        break;
    }
    default:
        d = 1 << 20;
    }
    return sdf_modifiers(p, x, y, d);
}

static inline void to_local(const prim_t *p, int32_t wx, int32_t wy, int32_t *x, int32_t *y) {
    int32_t dx = wx - p->cx, dy = wy - p->cy;
    if (p->sn == 0 && p->cs == 16384) {
        *x = dx;
        *y = dy;
    } else {
        *x = (p->cs * dx + p->sn * dy) >> 14;
        *y = (-p->sn * dx + p->cs * dy) >> 14;
    }
}
// The distance to p and its clip chain. A caller that only needs to know whether the point is at least `bound` away
// (and then steps by the distance) passes it: past it the clip cannot bring the point any closer, so the clip is
// not measured and the value returned is only a lower bound of the full distance.
static R_HOT int32_t sdf_upto(const scene_t *s, const prim_t *p, int32_t wx, int32_t wy, int32_t bound) {
    if (s_plain[p - s->p]) return sdf_rbox(p, wx - p->cx, wy - p->cy);
    int32_t x, y;
    to_local(p, wx, wy, &x, &y);
    int32_t d = sdf_local(p, x, y);
    if (d < bound && p->clip >= 0 && !s_noclip[p - s->p]) d = imax(d, sdf(s, &s->p[p->clip], wx, wy));
    return d;
}
static R_HOT int32_t sdf(const scene_t *s, const prim_t *p, int32_t wx, int32_t wy) {
    return sdf_upto(s, p, wx, wy, INT32_MAX);
}

static R_HOT void raster_qr(const scene_t *s, const prim_t *p, uint16_t *buf, int bx0, int bw, int ya, int yb,
                            int band_y0) {
    int n = p->a, m = p->b, ox = p->cx, oy = p->cy;
    int xs = imax(imax(p->bx0, bx0), ox), xe = imin(imin(p->bx1, bx0 + bw - 1), ox + n * m - 1);
    if (xs > xe) return;
    uint32_t a = ((uint32_t)p->alpha + 1) >> 3;
    uint16_t c = g_render_swap ? rgb565_bswap(p->color) : p->color;
    int mx0 = (xs - ox) / m;  // walk whole modules: no division per pixel
    for (int y = imax(ya, oy); y <= yb; y++) {
        int my = (y - oy) / m;
        if (my >= n) break;
        const uint8_t *row_mods = s->qr + my * n;
        uint16_t *row = buf + (y - band_y0) * bw - bx0;
        for (int mx = mx0, x = xs; x <= xe; mx++) {
            int end = imin(ox + (mx + 1) * m - 1, xe);
            if (row_mods[mx]) {
                if (a >= 32) {
                    for (int i = x; i <= end; i++) row[i] = c;
                } else {
                    for (int i = x; i <= end; i++)
                        row[i] = g_render_swap ? rgb565_bswap(rgb565_blend(p->color, rgb565_bswap(row[i]), a))
                                               : rgb565_blend(c, row[i], a);
                }
            }
            x = end + 1;
        }
    }
}

// ------------------------------------------------------------------ raster

static render_image_fn s_image_fn;
bool g_render_img_opaque_ok;
bool g_render_swap;
void render_set_image_fn(render_image_fn fn) { s_image_fn = fn; }

static void compute_bbox(scene_t *s, prim_t *p) {
    float ext = s_extent[p - s->p];
    if (ext >= 0) ext /= R_SCALE;
    if (ext == -2.f) return;
    float soft = p->soft / (float)Q;
    float hx, hy;
    if (ext < 0) {  // axis aligned box
        hx = p->a / (float)Q;
        hy = p->b / (float)Q;
    } else {
        hx = hy = ext;
    }
    float cx = p->cx / (float)Q, cy = p->cy / (float)Q;
    int x0 = (int)floorf(cx - hx - soft) - 1, x1 = (int)ceilf(cx + hx + soft) + 1;
    int y0 = (int)floorf(cy - hy - soft) - 1, y1 = (int)ceilf(cy + hy + soft) + 1;
    if (p->clip >= 0) {
        const prim_t *c = &s->p[p->clip];
        x0 = imax(x0, c->bx0);
        x1 = imin(x1, c->bx1);
        y0 = imax(y0, c->by0);
        y1 = imin(y1, c->by1);
    }
    p->bx0 = (int16_t)imax(x0, 0);
    p->by0 = (int16_t)imax(y0, 0);
    p->bx1 = (int16_t)imin(x1, R_W - 1);
    p->by1 = (int16_t)imin(y1, R_H - 1);
}

// Is the (convex) clip shape inside-by-margin at every corner of p's bbox? Then it clips nothing.
static bool clip_covers(const scene_t *s, const prim_t *p) {
    const prim_t *c = &s->p[p->clip];
    int32_t m = -(c->soft / 2 + Q);
    // Round shapes: the clip's (exact interior) distance at p's centre beats p's circumradius.
    float ext = s_extent[p - s->p];
    if (ext >= 0) ext /= R_SCALE;
    float rad = ext >= 0 ? ext : sqrtf((float)p->a * p->a + (float)p->b * p->b) / Q;
    if (sdf(s, c, p->cx, p->cy) <= m - (int32_t)((rad + p->soft / (float)Q) * Q)) return true;
    int32_t xs[2] = {p->bx0 * Q, (p->bx1 + 1) * Q}, ys[2] = {p->by0 * Q, (p->by1 + 1) * Q};
    for (int i = 0; i < 4; i++)
        if (sdf(s, c, xs[i & 1], ys[i >> 1]) > m) return false;
    return true;
}

static bool s_convex[R_MAX_PRIMS];  // prim and its whole clip chain are convex shapes

// Axis-aligned sharp box without modifiers (bands, bars): distances computed inline, no tracing. A row running
// along a horizontal edge would otherwise be traced a pixel or two at a time across its whole width.
static R_HOT void raster_rect(const prim_t *p, uint16_t *buf, int bx0, int bw, int xs, int xe, int ya, int yb, int band_y0) {
    int32_t half = p->soft / 2;
    int32_t inv = (int32_t)((256u << 16) / (uint32_t)p->soft);
    uint32_t solid_a = ((uint32_t)p->alpha + 1) >> 3;
    uint16_t col = g_render_swap ? rgb565_bswap(p->color) : p->color;
    // Columns whose centre is at least `half` inside the box's sides (there d <= -half on a deep row): one run.
    int32_t reach = p->a - half;
    int deep0 = imax(xs, (p->cx - reach - Q / 2 + Q - 1) >> 4), deep1 = imin(xe, (p->cx + reach - Q / 2) >> 4);
    for (int y = ya; y <= yb; y++) {
        int32_t qy = iabs(y * Q + Q / 2 - p->cy) - p->b;
        if (qy >= half) continue;
        uint16_t *row = buf + (y - band_y0) * bw - bx0;
        int32_t px = xs * Q + Q / 2 - p->cx;
        int run0 = xe + 1, run1 = xe + 1;
        if (qy <= -half && reach >= 0 && deep0 <= deep1) run0 = deep0, run1 = deep1 + 1;
        for (int x = xs; x <= xe; x++, px += Q) {
            if (x == run0) {
                fill_run(p, col, row, run0, run1, solid_a);
                x = run1 - 1;
                px += (run1 - 1 - run0) * Q;
                continue;
            }
            int32_t qx = iabs(px) - p->a;
            int32_t d = qx > 0 && qy > 0 ? len2(qx, qy) : imax(qx, qy);
            if (d <= -half && p->alpha == 255) row[x] = col;
            else put_px(p, col, row, x, d, half, inv, solid_a);
        }
    }
}

// Half-width of an upright rounded box (half sizes a, b incl. corner r) at |y| = ly, all Q4; -1 above/below it.
static inline int32_t rbox_ext(int32_t a, int32_t b, int32_t r, int32_t ly) {
    if (a <= 0 || b <= 0 || ly > b) return -1;
    int32_t ty = ly - (b - r);
    if (ty <= 0) return a;
    return (a - r) + (int32_t)isqrt((uint32_t)(r * r - ty * ty));
}
static inline int floor_div(int32_t v, int32_t d) { return v >= 0 ? v / d : -((-v + d - 1) / d); }

// Upright rounded-box outline (a stroke): per row the band's inner and outer extents are known in closed form,
// so the hollow is skipped and the band filled outright; only pixels that can be partly covered are evaluated.
// A pixel whose centre is within `half` of a boundary on every side of its row span is classified exactly: the
// extents at ly - half / ly + half bound the boundary over the pixel's height.
static R_HOT void raster_frame_row(const scene_t *s, const prim_t *p, uint16_t *buf, int bx0, int bw, int xs, int xe,
                                   int y, int band_y0, int32_t half, int32_t inv, uint32_t solid_a, uint16_t col) {
    int32_t py = y * Q + Q / 2, ly = iabs(py - p->cy), lo = imax(ly - half, 0), hi = ly + half;
    int32_t st = p->stroke, ai = p->a - st, bi = p->b - st, ri = imax(p->r - st, 0);
    int32_t eo_far = rbox_ext(p->a, p->b, p->r, lo);
    if (eo_far < 0) return;
    int32_t eo_near = rbox_ext(p->a, p->b, p->r, hi), ei_far = rbox_ext(ai, bi, ri, lo), ei_near = rbox_ext(ai, bi, ri, hi);
    int32_t b4 = eo_far + half + 2;
    int32_t b1 = ei_near >= 0 ? imax(ei_near - half, 0) : 0;
    int32_t b2 = ei_far >= 0 ? imin(imax(ei_far + half + 2, b1), b4) : b1;
    int32_t b3 = eo_near >= 0 ? imin(imax(eo_near - half, b2), b4) : b2;
    uint16_t *row = buf + (y - band_y0) * bw - bx0;
    int32_t zb[5] = {b1, b2, b3, b4, 0};
    for (int z = 0; z < 3; z++) {
        int32_t zlo = zb[z], zhi = zb[z + 1];
        if (zlo >= zhi) continue;
        int l0 = floor_div(p->cx - zhi - Q / 2, Q) + 1, l1 = floor_div(p->cx - zlo - Q / 2, Q);
        int r0 = -floor_div(-(p->cx + imax(zlo, 1) - Q / 2), Q), r1 = -floor_div(-(p->cx + zhi - Q / 2), Q) - 1;
        for (int side = 0; side < 2; side++) {
            int x0 = imax(side ? r0 : l0, xs), x1 = imin(side ? r1 : l1, xe);
            if (x0 > x1) continue;
            if (z == 1) { fill_run(p, col, row, x0, x1 + 1, solid_a); continue; }
            for (int x = x0; x <= x1; x++) put_px(p, col, row, x, sdf(s, p, x * Q + Q / 2, py), half, inv, solid_a);
        }
    }
}

static R_HOT void raster_frame(const scene_t *s, const prim_t *p, uint16_t *buf, int bx0, int bw, int xs, int xe, int ya, int yb,
                               int band_y0) {
    int32_t half = p->soft / 2;
    int32_t inv = (int32_t)((256u << 16) / (uint32_t)p->soft);
    uint32_t solid_a = ((uint32_t)p->alpha + 1) >> 3;
    uint16_t col = g_render_swap ? rgb565_bswap(p->color) : p->color;
    for (int y = ya; y <= yb; y++)
        raster_frame_row(s, p, buf, bx0, bw, xs, xe, y, band_y0, half, inv, solid_a, col);
}

static bool clip_is_full(const scene_t *s, const prim_t *p) {
    return p->clip < 0 || s_noclip[p - s->p];
}

static bool upright_rbox(const prim_t *p) {
    return p->kind == PK_RBOX && p->sn == 0 && p->cs == 16384;
}

static bool plain_rbox(const prim_t *p) {
    return p->r == 0 && p->soft == Q && !p->ncut && !p->has_sub && !p->stroke;
}

static bool outlined_rbox(const prim_t *p) {
    return p->stroke && !p->ncut && !p->has_sub;
}

static bool raster_special_rows(const scene_t *s, const prim_t *p, uint16_t *buf, int bx0, int bw, int xs, int xe,
                                int ya, int yb, int band_y0, bool upright) {
    if (!upright) return false;
    if (plain_rbox(p) && clip_is_full(s, p)) {
        raster_rect(p, buf, bx0, bw, xs, xe, ya, yb, band_y0);
        return true;
    }
    if (outlined_rbox(p) && clip_is_full(s, p)) {
        raster_frame(s, p, buf, bx0, bw, xs, xe, ya, yb, band_y0);
        return true;
    }
    return false;
}

static R_HOT int raster_convex_solid(const scene_t *s, const prim_t *p, uint16_t *row, int x, int xe, int32_t py,
                                     int32_t half, int32_t inv, uint32_t solid_a, uint16_t col) {
    int xr = xe;
    while (xr > x) {
        int32_t dr = sdf_upto(s, p, xr * Q + Q / 2, py, half);
        if (dr >= half) {
            xr -= ((dr - half) >> 4) + 1;
            continue;
        }
        if (dr <= -half) break;
        put_px(p, col, row, xr, dr, half, inv, solid_a);
        xr--;
    }
    fill_run(p, col, row, x, xr + 1, solid_a);
    return xe + 1;
}

static R_HOT int raster_inside_run(const scene_t *s, const prim_t *p, uint16_t *row, int x, int xe, int32_t d,
                                   int32_t py, int32_t half, int32_t inv, uint32_t solid_a, uint16_t col, bool convex) {
    if (convex) return raster_convex_solid(s, p, row, x, xe, py, half, inv, solid_a, col);
    int run = ((-d - half) >> 4) + 1;
    int end = imin(x + run, xe + 1);
    fill_run(p, col, row, x, end, solid_a);
    return end;
}

static R_HOT void raster_trace_row(const scene_t *s, const prim_t *p, uint16_t *row, int xs, int xe, int32_t py,
                                   int32_t half, int32_t inv, uint32_t solid_a, uint16_t col, int step, bool convex) {
    int x = xs;
    while (x <= xe) {
        int32_t d = sdf_upto(s, p, x * Q + Q / 2, py, half);
        if (d >= half) {
            x += ((d - half) >> 4) + 1;
            continue;
        }
        if (d <= -half) {
            x = raster_inside_run(s, p, row, x, xe, d, py, half, inv, solid_a, col, convex);
            if (convex) return;
            continue;
        }
        if (step > 1 && x + step <= xe) {
            int32_t d2 = sdf(s, p, (x + step) * Q + Q / 2, py);
            for (int k = 0; k < step; k++) {  // d + (d2 - d) * k / step, truncated toward zero, without the divide
                int32_t along = (d2 - d) * k;
                put_px(p, col, row, x + k, d + ((along + ((along >> 31) & (step - 1))) >> (step >> 1)), half, inv, solid_a);
            }
            x += step;
            continue;
        }
        put_px(p, col, row, x, d, half, inv, solid_a);
        x++;
    }
}

static R_HOT void raster_rows(const scene_t *s, const prim_t *p, uint16_t *buf, int bx0, int bw, int ya, int yb, int band_y0) {
    int xs = imax(p->bx0, bx0), xe = imin(p->bx1, bx0 + bw - 1);
    if (xs > xe) return;
    int32_t half = p->soft / 2;
    bool upright = upright_rbox(p);
    if (raster_special_rows(s, p, buf, bx0, bw, xs, xe, ya, yb, band_y0, upright)) return;
    int32_t inv = (int32_t)((256u << 16) / (uint32_t)p->soft);
    uint32_t solid_a = ((uint32_t)p->alpha + 1) >> 3;
    bool convex = s_convex[p - s->p];
    uint16_t col = g_render_swap ? rgb565_bswap(p->color) : p->color;
    int step = p->soft >= 8 * Q ? 4 : p->soft >= 4 * Q ? 2 : 1;
    for (int y = ya; y <= yb; y++) {
        uint16_t *row = buf + (y - band_y0) * bw - bx0;
        int32_t py = y * Q + Q / 2;
        if (upright && iabs(py - p->cy) - p->b >= half) continue;
        raster_trace_row(s, p, row, xs, xe, py, half, inv, solid_a, col, step, convex);
    }
}

static bool primitive_visible(const prim_t *p, int x0, int x1, int y0, int y1) {
    return p->by1 >= y0 && p->by0 <= y1 && p->bx1 >= x0 && p->bx0 <= x1 && p->alpha != 0 && p->kind < PK_TEXT;
}

bool render_raster_primitive(const scene_t *s, const prim_t *p, uint16_t *buf, int x0, int width, int y0, int y1,
                             bool drawn) {
    int x1 = x0 + width - 1;
    if (!primitive_visible(p, x0, x1, y0, y1)) return drawn;
    uint32_t t0 = R_NOW();
    if (p->kind == PK_SPRITE) {
        g_render_img_opaque_ok = !drawn && s->bg == 0;
        if (s_image_fn) s_image_fn(p, buf, x0, width, imax(p->by0, y0), imin(p->by1, y1), y0);
    } else if (p->kind == PK_QR) {
        raster_qr(s, p, buf, x0, width, imax(p->by0, y0), imin(p->by1, y1), y0);
    } else {
        raster_rows(s, p, buf, x0, width, imax(p->by0, y0), imin(p->by1, y1), y0);
    }
    if (p->kind == PK_SPRITE) image_us += R_NOW() - t0;
    else shapes_us += R_NOW() - t0;
    return true;
}

static void canvas_geometry(scene_t *s) {
    if (s->raster_ready) return;
    s->raster_ready = true;
    for (int i = 0; i < s->n; i++) {
        prim_t *p = &s->p[i];
        p->cx /= R_SCALE;
        p->cy /= R_SCALE;
        if (p->kind >= PK_SPRITE) {
            p->bx0 /= R_SCALE;
            p->by0 /= R_SCALE;
            p->bx1 /= R_SCALE;
            p->by1 /= R_SCALE;
            continue;
        }
        p->a /= R_SCALE;
        p->b /= R_SCALE;
        p->r /= R_SCALE;
        if (p->kind == PK_HEART) {  // unit K = a; the lobes' offset and radius follow from it
            p->b = (p->a * 55) / 100;
            p->r = (p->a * 3536) / 10000;
        }
        p->soft = imax(Q, p->soft / R_SCALE);
        p->sub_x /= R_SCALE;
        p->sub_y /= R_SCALE;
        p->sub_r /= R_SCALE;
        if (p->stroke) p->stroke = imax(Q / 2, p->stroke / R_SCALE);
        for (int j = 0; j < p->ncut; j++) p->cut_c[j] /= R_SCALE;
    }
}

static void prepare_clip_cache(scene_t *s) {
    for (int i = 0; i < s->n; i++) s_noclip[i] = false;
    for (int i = 0; i < s->n; i++)
        s_noclip[i] = s->p[i].clip >= 0 && s->p[i].kind < PK_SPRITE && s->p[i].bx0 <= s->p[i].bx1 &&
                      clip_covers(s, &s->p[i]);
}

static void prepare_plain_cache(scene_t *s) {
    for (int i = 0; i < s->n; i++) {
        const prim_t *p = &s->p[i];
        s_plain[i] = p->kind == PK_RBOX && p->sn == 0 && p->cs == 16384 && !p->ncut && !p->has_sub && !p->stroke &&
                     (p->clip < 0 || s_noclip[i]);
    }
}

static void prepare_convex_cache(scene_t *s) {
    for (int i = 0; i < s->n; i++) {
        const prim_t *p = &s->p[i];
        s_convex[i] = p->kind == PK_RBOX && !p->has_sub && !p->stroke &&
                      (p->clip < 0 || s_noclip[i] || s_convex[p->clip]);
    }
}

uint32_t render_raster_prepare(scene_t *s, bool swap) {
    image_us = shapes_us = glass_us = 0;
    uint32_t tp = R_NOW();
    canvas_geometry(s);
    g_render_swap = swap;
    if (!s_sqrt_t[1]) isqrt_init();
    for (int i = 0; i < s->n; i++) compute_bbox(s, &s->p[i]);
    render_dots_prepare(s);
    memset(s_plain, 0, sizeof s_plain);  // the clip cache measures with the general distance
    prepare_clip_cache(s);
    prepare_plain_cache(s);
    prepare_convex_cache(s);
    render_glass_prepare(s);
    return R_NOW() - tp;
}

void render_raster_get_timings(render_state_t *st) {
    st->image_us = image_us;
    st->shapes_us = shapes_us;
    st->glass_us = glass_us;
}
