#include "render_internal.h"

#include "font.h"
#include "icon_data.h"

#include <math.h>
#include <string.h>

static inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

static void render_text_glyph(const font_t *f, const font_glyph_t *g, uint16_t *out, int x0, int y0, int xe, int ye,
                              int width, int gx, int gy, const uint8_t alpha[16], uint16_t color, uint16_t source_color,
                              bool swap) {
    if (!g->w || gx > xe || gx + g->w <= x0) return;
    int stride = (g->w + 1) / 2;
    for (int y = imax(gy, y0); y <= imin(gy + g->h - 1, ye); y++) {
        const uint8_t *src = f->px + g->off + (y - gy) * stride;
        uint16_t *row = out + (y - y0) * width - x0;
        for (int x = imax(gx, x0); x <= imin(gx + g->w - 1, xe); x++) {
            int k = x - gx, v = (src[k >> 1] >> ((k & 1) * 4)) & 15;
            if (!v) continue;
            uint32_t a = alpha[v];
            uint16_t *d = &row[x];
            if (a >= 32) *d = color;
            else if (!swap) *d = rgb565_blend(color, *d, a);
            else *d = rgb565_bswap(rgb565_blend(source_color, rgb565_bswap(*d), a));
        }
    }
}

static inline int icon_axis_q8(int pixel, int origin_q4, int size_q4) {
    int q = (((pixel * 16 + 8 - origin_q4) * ICON_MASK_SIZE * 256) / size_q4) - 128;
    if (q < 0) return 0;
    int max = (ICON_MASK_SIZE - 1) << 8;
    return q > max ? max : q;
}

static inline int icon_floor_q4(int q4) { return q4 >= 0 ? q4 / 16 : -((-q4 + 15) / 16); }
static inline int icon_ceil_q4(int q4) { return q4 >= 0 ? (q4 + 15) / 16 : -((-q4) / 16); }

static inline int icon_mask_sample(const uint8_t *mask, int x, int y) {
    int i = y * ICON_MASK_SIZE + x;
    return (mask[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
}

static uint32_t icon_mask_coverage(const uint8_t *mask, int xq, int yq) {
    int x0 = xq >> 8, y0 = yq >> 8;
    int x1 = imin(x0 + 1, ICON_MASK_SIZE - 1), y1 = imin(y0 + 1, ICON_MASK_SIZE - 1);
    int fx = xq & 255, fy = yq & 255;
    int top = icon_mask_sample(mask, x0, y0) * (256 - fx) + icon_mask_sample(mask, x1, y0) * fx;
    int bottom = icon_mask_sample(mask, x0, y1) * (256 - fx) + icon_mask_sample(mask, x1, y1) * fx;
    return (uint32_t)((top * (256 - fy) + bottom * fy + 32768) >> 16);
}

static inline void icon_blend(uint16_t *dst, uint16_t color, uint16_t source_color, uint32_t alpha, bool swap) {
    if (alpha >= 32) *dst = color;
    else if (!swap) *dst = rgb565_blend(color, *dst, alpha);
    else *dst = rgb565_bswap(rgb565_blend(source_color, rgb565_bswap(*dst), alpha));
}

static void icon_composite_pixel(uint16_t *dst, uint16_t color, uint16_t source_color, uint8_t opacity,
                                 uint32_t coverage, bool swap) {
    if (!coverage) return;
    uint32_t alpha = (coverage * opacity * 32u + 1912u) / (15u * 255u);
    if (alpha) icon_blend(dst, color, source_color, alpha, swap);
}

static void render_icon_primitive(const prim_t *p, uint16_t *out, int x0, int y0, int width, int rows, bool swap) {
    if (!p->alpha || p->b < 0 || p->b >= ICON_COUNT || p->a <= 0) return;
    const uint8_t *mask = g_icon_masks[p->b];
    int size = p->a, half = size / 2;
    int left = p->sub_x - half, top = p->sub_y - half;
    int xs = imax(x0, icon_floor_q4(left)), xe = imin(x0 + width - 1, icon_ceil_q4(p->sub_x + size - half) - 1);
    int ys = imax(y0, icon_floor_q4(top)), ye = imin(y0 + rows - 1, icon_ceil_q4(p->sub_y + size - half) - 1);
    if (xs > xe || ys > ye) return;
    // Each column samples the same source coordinate on every row of this window.
    uint16_t xq[xe - xs + 1];
    for (int x = xs; x <= xe; x++) xq[x - xs] = (uint16_t)icon_axis_q8(x, left, size);
    uint16_t source_color = p->color, color = swap ? rgb565_bswap(source_color) : source_color;
    for (int y = ys; y <= ye; y++) {
        int syq = icon_axis_q8(y, top, size);
        uint16_t *row = out + (y - y0) * width - x0;
        for (int x = xs; x <= xe; x++) {
            uint32_t coverage = icon_mask_coverage(mask, xq[x - xs], syq);
            icon_composite_pixel(&row[x], color, source_color, p->alpha, coverage, swap);
        }
    }
}

static void render_text_primitive(const scene_t *s, const prim_t *p, uint16_t *out, int x0, int y0, int width, int rows,
                                 bool swap) {
    int xe = x0 + width - 1, ye = y0 + rows - 1;
    const font_t *f = &g_fonts[p->r];
    int base = p->sub_y;
    if (base + f->descent + 2 < y0 || base - f->ascent - 2 > ye) return;
    int pen = p->sub_x * 16;
    uint8_t alpha[16];
    for (unsigned v = 0; v < 16; v++)
        alpha[v] = (uint8_t)((v * p->alpha * 32 + 15 * 255 / 2) / (15 * 255));
    uint16_t c = swap ? rgb565_bswap(p->color) : p->color;
    for (int i = 0; i < p->b; i++) {
        const font_glyph_t *g = &f->g[s->text[p->a + i]];
        int gx = ((pen + 8) >> 4) + g->x, gy = base - g->top;
        pen += g->adv + p->sub_r + (i + 1 < p->b ? font_kern(f, s->text[p->a + i], s->text[p->a + i + 1]) : 0);
        render_text_glyph(f, g, out, x0, y0, xe, ye, width, gx, gy, alpha, c, p->color, swap);
    }
}

void render_text_overlay(const scene_t *s, uint16_t *out, int x0, int y0, int width, int rows, bool swap) {
    for (int n = 0; n < s->n; n++) {
        const prim_t *p = &s->p[n];
        if (p->kind == PK_ICON) render_icon_primitive(p, out, x0, y0, width, rows, swap);
        else if (p->kind == PK_TEXT && p->alpha) render_text_primitive(s, p, out, x0, y0, width, rows, swap);
    }
}

void sc_edge(scene_t *s, int w, int r, uint32_t rgb) {
    s->edge_w = (uint8_t)imax(0, imin(w, 255));
    s->edge_r = (uint8_t)imax(0, imin(r, 255));
    s->edge_color = rgb565(rgb);
}

// Coverage (0..32) of the disc of radius r around (cx, cy) at pixel (x, y): the distance to the circle is
// linearised, (r^2 - d^2) / 2r, which is exact at the rim and saturates correctly away from it.
static inline int disc_cover(int x, int y, int cx, int cy, int r) {
    if (r <= 0) return 0;
    int dx = 2 * (x - cx) + 1, dy = 2 * (y - cy) + 1;  // doubled: pixel centres
    int v = 16 + (4 * r * r - (dx * dx + dy * dy)) * 4 / r;
    return v < 0 ? 0 : v > 32 ? 32 : v;
}

// Per corner row j (distance from the panel's top or bottom edge): where the band's coverage is nonzero,
// [lo, hi), and the solid run inside it, [f0, f1), in columns from the left edge. The corners are mirror
// images, so one table serves all four; it is rebuilt when the band's width or radius changes (while it grows
// or retracts: every frame), so it is built from where the coverage of each circle starts and saturates, found by
// bisection, instead of from every pixel.
#define EDGE_R_MAX 128
static struct { uint8_t lo, f0, f1, hi; } s_edge_rows[EDGE_R_MAX];
static int s_edge_w = -1, s_edge_r = -1;

// The first column x < r of corner row j at which the squared distance to the centre of a disc of radius R stays
// within `limit` (it only shrinks as x grows); r when there is none. disc_cover() > 0 exactly where D < 4R^2 + 4R,
// and it is 32 where D <= 4R^2 - 4R.
static int first_within(int j, int r, int R, int limit) {
    if (R <= 0) return r;
    int dy = 2 * (j - r) + 1, lo = 0, hi = r;
    while (lo < hi) {
        int x = (lo + hi) / 2, dx = 2 * (x - r) + 1;
        if (dx * dx + dy * dy <= limit) hi = x;
        else lo = x + 1;
    }
    return lo;
}

static inline int edge_cover(int x, int j, int r, int w) {
    return disc_cover(x, j, r, r, r) - disc_cover(x, j, r, r, r - w);
}

static void edge_rows(int w, int r) {
    if (w == s_edge_w && r == s_edge_r) return;
    s_edge_w = w;
    s_edge_r = r;
    for (int j = 0; j < r; j++) {
        int in = r - w;
        int starts = first_within(j, r, r, 4 * r * r + 4 * r - 1), solid = first_within(j, r, r, 4 * r * r - 4 * r);
        int in_starts = first_within(j, r, in, 4 * in * in + 4 * in - 1), in_solid = first_within(j, r, in, 4 * in * in - 4 * in);
        int lo = starts, hi = in_solid;  // no coverage before the outer circle starts or once the inner one is solid
        while (lo < hi && edge_cover(lo, j, r, w) <= 0) lo++;
        while (hi > lo && edge_cover(hi - 1, j, r, w) <= 0) hi--;
        if (lo >= hi) lo = hi = 0;
        s_edge_rows[j].lo = (uint8_t)lo; s_edge_rows[j].hi = (uint8_t)hi;
        int f0 = solid, f1 = in_starts;  // solid where the outer circle is and the inner one is not yet
        if (f0 >= f1) f0 = f1 = 0;
        s_edge_rows[j].f0 = (uint8_t)f0; s_edge_rows[j].f1 = (uint8_t)f1;
    }
}

static inline void edge_px(uint16_t *d, int a, uint16_t col, uint16_t c, bool swap) {
    if (a <= 0) return;
    if (a >= 32) *d = c;
    else if (!swap) *d = rgb565_blend(col, *d, (uint32_t)a);
    else *d = rgb565_bswap(rgb565_blend(col, rgb565_bswap(*d), (uint32_t)a));
}

static bool edge_straight_sides(uint16_t *row, int y, int W, int H, int w, int r, int xa, int xb, uint16_t color) {
    if (y < r || y >= H - r) return false;
    for (int x = xa; x < imin(xb, w); x++) row[x] = color;
    for (int x = imax(xa, W - w); x < xb; x++) row[x] = color;
    return true;
}

static void edge_straight_top_bottom(uint16_t *row, int y, int W, int H, int w, int r, int xa, int xb, uint16_t color) {
    if (y < w || y >= H - w)
        for (int x = imax(xa, r); x < imin(xb, W - r); x++) row[x] = color;
}

static void edge_corner_side(uint16_t *row, int side, int y, int W, int xa, int xb, int r, int w, uint16_t color,
                             uint16_t source_color, bool swap, int cy, int lo, int hi, int f0, int f1) {
    int cx = side ? W - r : r;
    for (int k = lo; k < hi; k++) {
        if (k == f0 && f1 > f0) {
            int xs = side ? W - f1 : f0, xe = side ? W - 1 - f0 : f1 - 1;
            for (int x = imax(xs, xa); x <= imin(xe, xb - 1); x++) row[x] = color;
            k = f1 - 1;
            continue;
        }
        int x = side ? W - 1 - k : k;
        if (x < xa || x >= xb) continue;
        edge_px(&row[x], disc_cover(x, y, cx, cy, r) - disc_cover(x, y, cx, cy, r - w), source_color, color, swap);
    }
}

static void edge_corner_row(uint16_t *row, int y, int W, int H, int xa, int xb, int r, int w, uint16_t color,
                            uint16_t source_color, bool swap) {
    int jr = y < r ? y : H - 1 - y, cy = y < r ? r : H - r;
    int lo = s_edge_rows[jr].lo, hi = s_edge_rows[jr].hi;
    if (lo >= hi) return;
    int f0 = s_edge_rows[jr].f0, f1 = s_edge_rows[jr].f1;
    for (int side = 0; side < 2; side++)
        edge_corner_side(row, side, y, W, xa, xb, r, w, color, source_color, swap, cy, lo, hi, f0, f1);
}

void render_edge_overlay(const scene_t *s, uint16_t *out, int x0, int y0, int width, int rows, bool swap) {
    int w = s->edge_w;
    if (!w) return;
    const int W = R_W * R_SCALE, H = R_H * R_SCALE, r = render_edge_radius(w, s->edge_r);
    edge_rows(w, r);
    uint16_t color = swap ? rgb565_bswap(s->edge_color) : s->edge_color;
    int xa = imax(x0, 0), xb = imin(x0 + width, W);
    for (int j = 0; j < rows; j++) {
        int y = y0 + j;
        uint16_t *row = out + j * width - x0;
        if (edge_straight_sides(row, y, W, H, w, r, xa, xb, color)) continue;
        edge_straight_top_bottom(row, y, W, H, w, r, xa, xb, color);
        edge_corner_row(row, y, W, H, xa, xb, r, w, color, s->edge_color, swap);
    }
}

int render_edge_radius(int w, int r) {
    if (r < w) r = w;
    return r > EDGE_R_MAX ? EDGE_R_MAX : r;
}

int render_edge_reach(int w, int r, int y0, int y1) {
    const int W = R_W * R_SCALE, H = R_H * R_SCALE;
    if (!w) return 0;
    r = render_edge_radius(w, r);
    if (y0 < w || y1 > H - w) return W;
    return y0 < r || y1 > H - r ? r : w;
}

// Corner rows j0..j1 (counted from the top or bottom edge): the columns [*x0, *x1) from the left edge where the coverage
// of the band of width `hi` differs from that of width `lo`. Coverage is the outer disc's minus the inner disc's (radius
// r - w), so it differs where the inner disc of r - lo is not yet zero and the one of r - hi is not solid yet. Both
// limits only reach further out on rows nearer the edge (smaller j): the largest j bounds the start, the smallest the end.
static void corner_change(int j0, int j1, int r, int lo, int hi, int *x0, int *x1) {
    int big = r - lo, small = r - hi;
    *x0 = first_within(j1, r, big, 4 * big * big + 4 * big - 1);
    *x1 = first_within(j0, r, small, 4 * small * small - 4 * small);
}

static void grow_hull(edge_change_t *c, int x0, int x1) {
    if (x0 >= x1) return;
    if (c->x0 >= c->x1) c->x0 = x0, c->x1 = x1;
    else c->x0 = imin(c->x0, x0), c->x1 = imax(c->x1, x1);
}

edge_change_t render_edge_change(int w0, int w1, int r, int y0, int y1) {
    const int H = R_H * R_SCALE;
    int lo = imin(w0, w1), hi = imax(w0, w1);
    r = render_edge_radius(hi, r);
    edge_change_t c = {0, 0, 0, 0, (int16_t)r};
    int j0 = r, j1 = -1;  // the corner rows of the band by their distance from the top or bottom edge
    int top_end = imin(y1, r), bottom_start = imax(y0, H - r);
    if (y0 < top_end) j0 = imin(j0, y0), j1 = imax(j1, top_end - 1);
    if (bottom_start < y1) j0 = imin(j0, H - y1), j1 = imax(j1, H - 1 - bottom_start);
    if (j1 >= 0) {
        int x0, x1;
        corner_change(j0, j1, r, lo, hi, &x0, &x1);
        grow_hull(&c, x0, x1);
    }
    if (imax(y0, r) < imin(y1, H - r)) grow_hull(&c, lo, hi);  // the straight sides
    int t0 = imax(y0, lo), t1 = imin(y1, hi), b0 = imax(y0, H - hi), b1 = imin(y1, H - lo);  // the straight top and bottom
    int s0 = H, s1 = 0;
    if (t0 < t1) s0 = t0, s1 = t1;
    if (b0 < b1) s0 = imin(s0, b0), s1 = imax(s1, b1);
    if (s0 < s1) c.sy0 = (int16_t)s0, c.sy1 = (int16_t)s1;
    return c;
}

static inline void signature_mix(uint32_t *hash, uint32_t value) {
    *hash = (*hash ^ value) * 16777619u;
}

static bool signature_hash_primitive(const scene_t *s, const prim_t *p, uint32_t *hash) {
    if (!p->alpha) return false;
    if (p->kind == PK_ICON) {
        if (p->b < 0 || p->b >= ICON_COUNT) return false;
        signature_mix(hash, (uint32_t)p->sub_x); signature_mix(hash, (uint32_t)p->sub_y);
        signature_mix(hash, (uint32_t)p->a); signature_mix(hash, (uint32_t)p->b);
        signature_mix(hash, p->alpha); signature_mix(hash, p->color);
        return true;
    }
    if (p->kind != PK_TEXT) return false;
    signature_mix(hash, (uint32_t)p->sub_x); signature_mix(hash, (uint32_t)p->sub_y);
    signature_mix(hash, (uint32_t)p->sub_r); signature_mix(hash, p->alpha); signature_mix(hash, p->color);
    signature_mix(hash, (uint32_t)p->r); signature_mix(hash, (uint32_t)p->b);
    for (int i = 0; i < p->b; i++) signature_mix(hash, s->text[p->a + i]);
    return true;
}

static void signature_add_bounds(const scene_t *s, const prim_t *p, int *x0, int *y0, int *x1, int *y1) {
    int bx0 = p->bx0, by0 = p->by0, bx1 = p->bx1, by1 = p->by1;
    if (!s->raster_ready) bx0 /= R_SCALE, by0 /= R_SCALE, bx1 /= R_SCALE, by1 /= R_SCALE;
    bx1 += 2;  // cover the bilinear native-pixel neighbour
    by1 += 2;
    *x0 = imin(*x0, bx0); *y0 = imin(*y0, by0);
    *x1 = imax(*x1, imin(bx1, R_W)); *y1 = imax(*y1, imin(by1, R_H));
}

uint32_t render_text_signature(const scene_t *s, int *x0, int *y0, int *x1, int *y1) {
    uint32_t h = 2166136261u;
    *x0 = *y0 = R_W;
    *x1 = *y1 = 0;
    for (int n = 0; n < s->n; n++) {
        const prim_t *p = &s->p[n];
        if (signature_hash_primitive(s, p, &h)) signature_add_bounds(s, p, x0, y0, x1, y1);
    }
    if (*x0 >= *x1 || *y0 >= *y1) *x0 = *y0 = *x1 = *y1 = 0;
    return h;
}


bool render_overlay_item(const scene_t *s, const prim_t *p, render_overlay_item_t *item) {
    uint32_t h = 2166136261u;
    if (!signature_hash_primitive(s, p, &h)) return false;
    signature_mix(&h, (uint32_t)p->kind);
    int x0 = R_W, y0 = R_H, x1 = 0, y1 = 0;
    signature_add_bounds(s, p, &x0, &y0, &x1, &y1);
    x0 = imax(x0, 0); y0 = imax(y0, 0);
    if (x0 >= x1 || y0 >= y1) return false;
    *item = (render_overlay_item_t){h, x0, y0, x1, y1};
    return true;
}
