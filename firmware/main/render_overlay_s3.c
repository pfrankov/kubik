#include "render_overlay_s3.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "icon_data.h"
#include "render_internal.h"
#include <string.h>

static int maximum(int a, int b) { return a > b ? a : b; }
static int minimum(int a, int b) { return a < b ? a : b; }
static int floor_div(int value, int divisor) { return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor); }
static int native_floor(int design) { return floor_div(design * UI_SCALE_NUM, UI_SCALE_DEN); }
static int native_ceil(int design) { return -native_floor(-design); }
// Callers clip physical coordinates before sampling. For k=2*native+1,
// floor(k*3840/23)=167*k-ceil(k/23); the small reciprocal is exact after
// one correction, with every product fitting uint32_t.
_Static_assert(UI_SCALE_NUM == 23 && UI_SCALE_DEN == 30, "Native axis algebra requires the S3 viewport scale");
static int design_q8(int native) {
    unsigned k = (unsigned)native * 2u + 1u;
    uint16_t n = (uint16_t)(k + 22u);
    unsigned q = ((uint32_t)n * 2850u) >> 16;
    q -= q * 23u > n;
    return (int)(167u * k - q) - 128;
}
static int coverage32(int value) { return value < 0 ? 0 : value > 32 ? 32 : value; }

// Builders own conservative design bounds; projection changes only their grid.
// Two output pixels retain all source-filter support at either boundary.
static bool primitive_intersects(const scene_t *scene, const prim_t *p, int x0, int y0, int width, int rows) {
    int xa, xb, ya, yb;
    if (scene->raster_ready) {
        xa = p->bx0 * R_SCALE; xb = (p->bx1 + 1) * R_SCALE;
        ya = p->by0 * R_SCALE; yb = (p->by1 + 1) * R_SCALE;
    } else {
        xa = native_floor(p->bx0); xb = native_ceil(p->bx1 + 1);
        ya = native_floor(p->by0); yb = native_ceil(p->by1 + 1);
    }
    return xa - 2 < x0 + width && xb + 2 > x0 && ya - 2 < y0 + rows && yb + 2 > y0;
}

// Exact rounded alpha for coverage0..3840 and opacity0..255. The reciprocal
// products fit uint32_t; each estimate is at most one above the quotient.
static inline __attribute__((always_inline)) unsigned coverage_alpha(unsigned coverage_q8, uint8_t opacity) {
    if (opacity == 255) {
        uint16_t n = (uint16_t)(coverage_q8 + 60u);
        unsigned q = ((uint32_t)n * 547u) >> 16;
        return q - (q * 120u > n);
    }
    uint32_t n = (uint32_t)(uint16_t)coverage_q8 * opacity + 15300u;
    unsigned q = (n * 549u) >> 24;
    return q - (q * 30600u > n);
}

static inline __attribute__((always_inline)) void composite_prepared(uint16_t *dst, uint16_t color, uint16_t wire,
                                                                    unsigned alpha, bool swap) {
    if (!alpha) return;
    if (alpha >= 32) *dst = wire;
    else if (swap) *dst = rgb565_bswap(rgb565_blend(color, rgb565_bswap(*dst), alpha));
    else *dst = rgb565_blend(color, *dst, alpha);
}

// Coverage stays Q8 until opacity is applied; all arithmetic fits uint32_t.
static inline __attribute__((always_inline)) void composite(uint16_t *dst, uint16_t color, uint8_t opacity, unsigned coverage_q8, bool swap) {
    composite_prepared(dst, color, swap ? rgb565_bswap(color) : color, coverage_alpha(coverage_q8, opacity), swap);
}

// Endpoints are 4-bit coverage or its Q8 interpolation (at most3840).
// Signed differences keep a decreasing edge exact without an extra multiply.
static inline __attribute__((always_inline)) unsigned linear_q8(unsigned a, unsigned b, unsigned fraction) {
    int16_t delta = (int16_t)((int)b - (int)a), weight = (int16_t)fraction;
    return (unsigned)((int)a * 256 + delta * weight);
}

static unsigned bilinear(unsigned a, unsigned b, unsigned c, unsigned d, int fx, int fy) {
    unsigned top = linear_q8(a, b, (unsigned)fx), bottom = linear_q8(c, d, (unsigned)fx);
    return (linear_q8(top, bottom, (unsigned)fy) + 128) >> 8;
}

// A uint8_t glyph width projects to at most this many physical columns,
// including the one-source-pixel filter support at each side.
#define GLYPH_COLUMNS (((UINT8_MAX + 2) * UI_SCALE_NUM + UI_SCALE_DEN - 1) / UI_SCALE_DEN + 1)
typedef struct {
    uint8_t first[GLYPH_COLUMNS], fraction[GLYPH_COLUMNS];
    uint16_t first_used, last_used;
} glyph_columns_t;

static inline __attribute__((always_inline)) void glyph_decode_row(const font_t *font, const font_glyph_t *glyph,
                                                                  int y, uint8_t pixels[UINT8_MAX + 2]) {
    pixels[0] = pixels[glyph->w + 1] = 0;
    if (y < 0 || y >= glyph->h) { memset(pixels + 1, 0, glyph->w); return; }
    const uint8_t *src = font->px + glyph->off + y * ((glyph->w + 1) / 2);
    for (int x = 0; x < glyph->w; x++) pixels[x + 1] = (uint8_t)((src[x / 2] >> ((x & 1) * 4)) & 15);
}

static inline __attribute__((always_inline)) bool glyph_prepare_columns(glyph_columns_t *columns, int xa, int xb,
                                                                        int gx, int glyph_width) {
    columns->first_used = UINT8_MAX + 1; columns->last_used = 0;
    for (int x = xa; x < xb; x++) {
        int sx = design_q8(x) - gx * 256, ix = floor_div(sx, 256);
        columns->first[x - xa] = columns->fraction[x - xa] = 0;
        if (ix < -1 || ix >= glyph_width) continue;
        unsigned index = (unsigned)(ix + 1);
        columns->first[x - xa] = (uint8_t)index;
        columns->fraction[x - xa] = (uint8_t)(sx - ix * 256);
        columns->first_used = (uint16_t)minimum(columns->first_used, index);
        columns->last_used = (uint16_t)maximum(columns->last_used, index + 1);
    }
    return columns->first_used <= columns->last_used;
}

static inline __attribute__((always_inline)) void glyph_vertical_row(uint16_t coverage[UINT8_MAX + 2],
        const uint8_t *top, const uint8_t *bottom, int first, int last, unsigned fraction) {
    // Out-of-source columns map to the transparent prefix with fraction zero.
    coverage[0] = coverage[1] = 0;
    for (int x = first; x <= last; x++) coverage[x] = (uint16_t)linear_q8(top[x], bottom[x], fraction);
}

typedef struct { uint16_t color, wire; uint8_t opacity, solid_alpha; bool swap; } glyph_brush_t;

static R_HOT void glyph_composite_row(const glyph_columns_t *columns, const uint16_t *vertical,
                                      uint16_t *dst, int count, const glyph_brush_t *brush) {
    for (int x = 0; x < count; x++) {
        unsigned index = columns->first[x], a = vertical[index], b = vertical[index + 1];
        if (!(a | b)) continue;
        if (a == 3840 && b == 3840) {
            composite_prepared(&dst[x], brush->color, brush->wire, brush->solid_alpha, brush->swap);
            continue;
        }
        unsigned coverage = (linear_q8(a, b, columns->fraction[x]) + 128) >> 8;
        if (coverage) composite(&dst[x], brush->color, brush->opacity, coverage, brush->swap);
    }
}

static R_HOT void glyph_overlay(const font_t *font, const font_glyph_t *glyph, uint16_t *out,
                          int x0, int y0, int width, int rows, int stride, int gx, int gy, uint16_t color, uint8_t opacity, bool swap) {
    if (!glyph->w || !glyph->h) return;
    int xa = maximum(maximum(x0, 0), native_floor(gx - 1));
    int xb = minimum(minimum(x0 + width, UI_OUTPUT_W), native_ceil(gx + glyph->w + 1));
    int ya = maximum(maximum(y0, 0), native_floor(gy - 1));
    int yb = minimum(minimum(y0 + rows, UI_OUTPUT_H), native_ceil(gy + glyph->h + 1));
    if (xa >= xb || ya >= yb) return;
    glyph_columns_t columns;
    if (!glyph_prepare_columns(&columns, xa, xb, gx, glyph->w)) return;
    glyph_brush_t brush = {.color = color, .wire = swap ? rgb565_bswap(color) : color,
        .opacity = opacity, .solid_alpha = (uint8_t)coverage_alpha(3840, opacity), .swap = swap};
    uint16_t vertical[UINT8_MAX + 2];
    uint8_t pixels[2][UINT8_MAX + 2]; int keys[2] = {INT32_MIN, INT32_MIN};
    for (int y = ya; y < yb; y++) {
        int sy = design_q8(y) - gy * 256, iy = floor_div(sy, 256), fy = sy - iy * 256;
        int k = (unsigned)iy & 1u, next = k ^ 1;
        if (keys[k] != iy) { glyph_decode_row(font, glyph, iy, pixels[k]); keys[k] = iy; }
        if (keys[next] != iy + 1) { glyph_decode_row(font, glyph, iy + 1, pixels[next]); keys[next] = iy + 1; }
        glyph_vertical_row(vertical, pixels[k], pixels[next], columns.first_used, columns.last_used, (unsigned)fy);
        uint16_t *row = out + (y - y0) * stride + xa - x0;
        glyph_composite_row(&columns, vertical, row, xb - xa, &brush);
    }
}

static R_HOT void text_overlay(const scene_t *scene, const prim_t *p, uint16_t *out,
                         int x0, int y0, int width, int rows, int stride, bool swap) {
    const font_t *font = &g_fonts[p->r];
    int pen = p->sub_x * 16;
    for (int i = 0; i < p->b; i++) {
        pen += scene->text_kern[p->a+i];
        unsigned index = scene->text[p->a + i];
        if(index>=R_EMOJI_TOKEN)
            render_emoji_overlay(scene,(uint16_t)index,out,x0,y0,width,rows,stride,(pen+8)>>4,
                p->sub_y-(font->cap+R_EMOJI_EDGE)/2,p->alpha,swap);
        else {
            const font_glyph_t *glyph=&font->g[index];
            int gx=((pen+8)>>4)+glyph->x,gy=p->sub_y-glyph->top;
            glyph_overlay(font,glyph,out,x0,y0,width,rows,stride,gx,gy,p->color,p->alpha,swap);
        }
        pen+=render_token_advance(font,(uint16_t)index)+p->sub_r;
    }
}

static unsigned icon_sample(const uint8_t *mask, int x, int y) {
    x = maximum(0, minimum(ICON_MASK_SIZE - 1, x)); y = maximum(0, minimum(ICON_MASK_SIZE - 1, y));
    int index = y * ICON_MASK_SIZE + x;
    return (mask[index / 2] >> ((index & 1) ? 0 : 4)) & 15;
}

static int icon_axis(int pixel, int origin, int size) {
    // Inverse scale at the physical pixel centre, then into the original mask.
    int center_q8 = design_q8(pixel) + 128;
    return floor_div((center_q8 - origin * 16) * ICON_MASK_SIZE * 16, size) - 128;
}

static R_HOT void icon_overlay(const prim_t *p, uint16_t *out, int x0, int y0, int width, int rows, int stride, bool swap) {
    if (p->a <= 0 || p->b < 0 || p->b >= ICON_COUNT) return;
    int left = p->sub_x - p->a / 2, top = p->sub_y - p->a / 2;
    int xa = maximum(maximum(x0, 0), native_floor(floor_div(left, 16)));
    int xb = minimum(minimum(x0 + width, UI_OUTPUT_W), native_ceil(-floor_div(-left - p->a, 16)));
    int ya = maximum(maximum(y0, 0), native_floor(floor_div(top, 16)));
    int yb = minimum(minimum(y0 + rows, UI_OUTPUT_H), native_ceil(-floor_div(-top - p->a, 16)));
    if (xa >= xb || ya >= yb) return;
    const uint8_t *mask = g_icon_masks[p->b];
    int32_t xq[UI_OUTPUT_W];
    for (int x = xa; x < xb; x++) xq[x] = icon_axis(x, left, p->a);
    for (int y = ya; y < yb; y++) {
        int sy = icon_axis(y, top, p->a), iy = floor_div(sy, 256), fy = sy - iy * 256;
        uint16_t *row = out + (y - y0) * stride;
        for (int x = xa; x < xb; x++) {
            int sx = xq[x], ix = floor_div(sx, 256), fx = sx - ix * 256;
            unsigned coverage = bilinear(icon_sample(mask, ix, iy), icon_sample(mask, ix + 1, iy),
                                          icon_sample(mask, ix, iy + 1), icon_sample(mask, ix + 1, iy + 1), fx, fy);
            if (coverage) composite(&row[x - x0], p->color, p->alpha, coverage, swap);
        }
    }
}

R_HOT void render_s3_text_overlay(const scene_t *scene, uint16_t *out, int x0, int y0, int width, int rows, int stride, bool swap) {
    for (int i = 0; i < scene->n; i++) {
        const prim_t *p = &scene->p[i];
        if (!p->alpha || !primitive_intersects(scene, p, x0, y0, width, rows)) continue;
        if (p->kind == PK_TEXT) text_overlay(scene, p, out, x0, y0, width, rows, stride, swap);
        else if (p->kind == PK_ICON) icon_overlay(p, out, x0, y0, width, rows, stride, swap);
    }
}

static inline void edge_composite(uint16_t *dst, uint16_t color, unsigned alpha, bool swap) {
    if (alpha >= 32) *dst = swap ? rgb565_bswap(color) : color;
    else if (swap) *dst = rgb565_bswap(rgb565_blend(color, rgb565_bswap(*dst), alpha));
    else *dst = rgb565_blend(color, *dst, alpha);
}

typedef struct { uint16_t color, wire, black; uint8_t alpha; bool swap; } qr_brush_t;
typedef uint32_t qr_pair_t __attribute__((__may_alias__));

static inline __attribute__((always_inline)) uint16_t qr_blend_pixel(const qr_brush_t *brush, uint16_t background) {
    if (!background) return brush->black;
    uint16_t source = brush->swap ? rgb565_bswap(background) : background;
    uint16_t result = rgb565_blend(brush->color, source, brush->alpha);
    return brush->swap ? rgb565_bswap(result) : result;
}

// Pair stores preserve arbitrary clipped/odd windows and both RGB565 orders.
static R_HOT void qr_paint_span(const qr_brush_t *brush, uint16_t *dst, int count) {
    if (count > 0 && ((uintptr_t)dst & 3u)) {
        *dst = brush->alpha >= 32 ? brush->wire : qr_blend_pixel(brush, *dst);
        dst++; count--;
    }
    uint32_t opaque = pair_of(brush->wire), black = pair_of(brush->black);
    qr_pair_t *pairs = (qr_pair_t *)(void *)dst;
    while (count >= 2) {
        if (brush->alpha >= 32) *pairs = opaque;
        else {
            uint32_t background = *pairs;
            if (!background) *pairs = black;
            else *pairs = qr_blend_pixel(brush, (uint16_t)background)
                        | ((uint32_t)qr_blend_pixel(brush, (uint16_t)(background >> 16)) << 16);
        }
        pairs++; count -= 2;
    }
    if (count) {
        dst = (uint16_t *)(void *)pairs;
        *dst = brush->alpha >= 32 ? brush->wire : qr_blend_pixel(brush, *dst);
    }
}

typedef struct {
    uint16_t *out;
    const uint8_t *modules;
    const qr_brush_t *brush;
    int x0, y0, stride, xa, xb, ya, yb, ox, oy, side, n;
} qr_window_t;

static int qr_boundary(int module, int side, int n) { return (module * side + n - 1) / n; }

static R_HOT void qr_paint_source_row(const qr_window_t *window, int my, int first_x, int last_x) {
    int ya = maximum(window->ya, window->oy + qr_boundary(my, window->side, window->n));
    int yb = minimum(window->yb, window->oy + qr_boundary(my + 1, window->side, window->n));
    if (ya >= yb) return;
    const uint8_t *source = window->modules + my * window->n;
    for (int mx = first_x; mx < last_x;) {
        while (mx < last_x && !source[mx]) mx++;
        int first = mx;
        while (mx < last_x && source[mx]) mx++;
        if (first == mx) break;
        int xa = maximum(window->xa, window->ox + qr_boundary(first, window->side, window->n));
        int xb = minimum(window->xb, window->ox + qr_boundary(mx, window->side, window->n));
        for (int y = ya; y < yb; y++)
            qr_paint_span(window->brush, window->out + (y - window->y0) * window->stride + xa - window->x0, xb - xa);
    }
}

static R_HOT void qr_overlay(const scene_t *scene, const prim_t *p, uint16_t *out,
                       int x0, int y0, int width, int rows, int stride, bool swap) {
    int ox = p->sub_x, oy = p->sub_y, side = p->b, n = p->a;
    if (side <= 0 || n <= 0 || !scene->qr) return;
    qr_window_t window = {.out = out, .modules = scene->qr, .x0 = x0, .y0 = y0, .stride = stride,
        .xa = maximum(maximum(x0, 0), ox), .xb = minimum(minimum(x0 + width, UI_OUTPUT_W), ox + side),
        .ya = maximum(maximum(y0, 0), oy), .yb = minimum(minimum(y0 + rows, UI_OUTPUT_H), oy + side),
        .ox = ox, .oy = oy, .side = side, .n = n};
    if (window.xa >= window.xb || window.ya >= window.yb) return;
    uint8_t alpha = (uint8_t)((p->alpha * 32u + 127u) / 255u);
    if (!alpha) return;
    uint16_t black = rgb565_blend(p->color, 0, alpha);
    qr_brush_t brush = {.color = p->color, .wire = swap ? rgb565_bswap(p->color) : p->color,
        .black = swap ? rgb565_bswap(black) : black, .alpha = alpha, .swap = swap};
    window.brush = &brush;
    int first_x = (window.xa - ox) * n / side, last_x = (window.xb - 1 - ox) * n / side + 1;
    int first_y = (window.ya - oy) * n / side, last_y = (window.yb - 1 - oy) * n / side + 1;
    for (int my = first_y; my < last_y; my++) qr_paint_source_row(&window, my, first_x, last_x);
}

static int disc(int dx, int dy, int radius) {
    return radius <= 0 ? 0 : coverage32(16 + (radius * radius - dx * dx - dy * dy) / radius);
}

static int edge_alpha(int x, int y, int radius, int thickness) {
    int dx = minimum(x * 16 + 8, (UI_OUTPUT_W - x) * 16 - 8);
    int dy = minimum(y * 16 + 8, (UI_OUTPUT_H - y) * 16 - 8);
    if (dx >= radius || dy >= radius) return coverage32(16 + (thickness - minimum(dx, dy)) * 2);
    dx -= radius; dy -= radius;
    return disc(dx, dy, radius) - disc(dx, dy, radius - thickness);
}


R_HOT void render_s3_edge_overlay(const scene_t *scene, uint16_t *out, int x0, int y0, int width, int rows, int stride, bool swap) {
    for (int i = 0; i < scene->n; i++)
        if (scene->p[i].kind == PK_QR && scene->p[i].alpha && primitive_intersects(scene, &scene->p[i], x0, y0, width, rows))
            qr_overlay(scene, &scene->p[i], out, x0, y0, width, rows, stride, swap);
    if (!scene->edge_w) return;
    int radius = (render_edge_radius(scene->edge_w, scene->edge_r) * UI_SCALE_NUM * 16 + UI_SCALE_DEN / 2) / UI_SCALE_DEN;
    int thickness = (scene->edge_w * UI_SCALE_NUM * 16 + UI_SCALE_DEN / 2) / UI_SCALE_DEN;
    int xa = maximum(0, x0), xb = minimum(UI_OUTPUT_W, x0 + width);
    for (int y = maximum(0, y0); y < minimum(UI_OUTPUT_H, y0 + rows); y++) {
        uint16_t *row = out + (y - y0) * stride;
        int reach = (maximum(radius, thickness) + 15) / 16 + 1;
        bool middle_clear = minimum(y * 16 + 8, (UI_OUTPUT_H - y) * 16 - 8) >= thickness + 8;
        for (int x = xa; x < xb; x++) {
            if (middle_clear && x >= reach && x < UI_OUTPUT_W - reach) x = UI_OUTPUT_W - reach;
            if (x >= xb) break;
            int alpha = edge_alpha(x, y, radius, thickness);
            if (!alpha) continue;
            uint16_t *dst = &row[x - x0];
            edge_composite(dst, scene->edge_color, (unsigned)alpha, swap);
        }
    }
}
static int scaled_q4(int value) { return (value * UI_SCALE_NUM * 16 + UI_SCALE_DEN / 2) / UI_SCALE_DEN; }

int render_s3_edge_reach(int width, int radius, int y0, int y1) {
    if (!width) return 0;
    int w = (scaled_q4(width) + 23) / 16;
    int r = (scaled_q4(render_edge_radius(width, radius)) + 23) / 16;
    if (y0 < w || y1 > UI_OUTPUT_H - w) return UI_OUTPUT_W;
    return y0 < r || y1 > UI_OUTPUT_H - r ? r : w;
}

edge_change_t render_s3_edge_change(int before, int after, int radius, int y0, int y1) {
    int outer = scaled_q4(render_edge_radius(maximum(before, after), radius));
    int old_width = scaled_q4(before), new_width = scaled_q4(after);
    int reach = (maximum(outer, maximum(old_width, new_width)) + 23) / 16;
    edge_change_t change = {0, 0, 0, 0, (int16_t)reach};
    int xa = reach, xb = 0, sy0 = UI_OUTPUT_H, sy1 = 0;
    for (int y = maximum(y0, 0); y < minimum(y1, UI_OUTPUT_H); y++) {
        for (int x = 0; x < reach; x++) {
            int a = before ? edge_alpha(x, y, outer, old_width) : 0;
            int b = after ? edge_alpha(x, y, outer, new_width) : 0;
            if (a == b) continue;
            xa = minimum(xa, x); xb = maximum(xb, x + 1);
        }
        int a = before ? edge_alpha(reach, y, outer, old_width) : 0;
        int b = after ? edge_alpha(reach, y, outer, new_width) : 0;
        if (a != b) { sy0 = minimum(sy0, y); sy1 = y + 1; }
    }
    if (xa < xb) { change.x0 = (int16_t)xa; change.x1 = (int16_t)xb; }
    if (sy0 < sy1) { change.sy0 = (int16_t)sy0; change.sy1 = (int16_t)sy1; }
    return change;
}
#endif
