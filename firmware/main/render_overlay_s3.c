#include "render_overlay_s3.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "icon_data.h"
#include <string.h>

static int maximum(int a, int b) { return a > b ? a : b; }
static int minimum(int a, int b) { return a < b ? a : b; }
static int floor_div(int value, int divisor) { return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor); }
static int native_floor(int design) { return floor_div(design * UI_SCALE_NUM, UI_SCALE_DEN); }
static int native_ceil(int design) { return -native_floor(-design); }
static int design_q8(int native) { return ((native * 2 + 1) * UI_SCALE_DEN * 128) / UI_SCALE_NUM - 128; }
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

// Coverage stays Q8 until opacity is applied; all arithmetic fits uint32_t.
static inline __attribute__((always_inline)) void composite(uint16_t *dst, uint16_t color, uint8_t opacity, unsigned coverage_q8, bool swap) {
    unsigned alpha = opacity == 255 ? (coverage_q8 + 60u) / 120u
                                   : (coverage_q8 * opacity * 32u + 489600u) / 979200u;
    if (!alpha) return;
    if (alpha >= 32) *dst = swap ? rgb565_bswap(color) : color;
    else if (swap) *dst = rgb565_bswap(rgb565_blend(color, rgb565_bswap(*dst), alpha));
    else *dst = rgb565_blend(color, *dst, alpha);
}

static unsigned bilinear(unsigned a, unsigned b, unsigned c, unsigned d, int fx, int fy) {
    unsigned top = a * (256 - fx) + b * fx, bottom = c * (256 - fx) + d * fx;
    return (top * (256 - fy) + bottom * fy + 128) >> 8;
}

// A uint8_t glyph width projects to at most this many physical columns,
// including the one-source-pixel filter support at each side.
#define GLYPH_COLUMNS (((UINT8_MAX + 2) * UI_SCALE_NUM + UI_SCALE_DEN - 1) / UI_SCALE_DEN + 1)
typedef struct {
    uint8_t first[GLYPH_COLUMNS], fraction[GLYPH_COLUMNS];
} glyph_columns_t;

static inline __attribute__((always_inline)) void glyph_decode_row(const font_t *font, const font_glyph_t *glyph,
                                                                  int y, uint8_t pixels[UINT8_MAX + 2]) {
    pixels[0] = pixels[glyph->w + 1] = 0;
    if (y < 0 || y >= glyph->h) { memset(pixels + 1, 0, glyph->w); return; }
    const uint8_t *src = font->px + glyph->off + y * ((glyph->w + 1) / 2);
    for (int x = 0; x < glyph->w; x++) pixels[x + 1] = (uint8_t)((src[x / 2] >> ((x & 1) * 4)) & 15);
}

static inline __attribute__((always_inline)) void glyph_prepare_columns(glyph_columns_t *columns, int xa, int xb,
                                                                        int gx, int glyph_width) {
    for (int x = xa; x < xb; x++) {
        int sx = design_q8(x) - gx * 256, ix = floor_div(sx, 256);
        bool valid = ix >= -1 && ix < glyph_width;
        columns->first[x - xa] = valid ? (uint8_t)(ix + 1) : 0;
        columns->fraction[x - xa] = valid ? (uint8_t)(sx - ix * 256) : 0;
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
    glyph_prepare_columns(&columns, xa, xb, gx, glyph->w);
    uint8_t pixels[2][UINT8_MAX + 2]; int keys[2] = {INT32_MIN, INT32_MIN};
    for (int y = ya; y < yb; y++) {
        int sy = design_q8(y) - gy * 256, iy = floor_div(sy, 256), fy = sy - iy * 256;
        int k = (unsigned)iy & 1u, next = k ^ 1;
        if (keys[k] != iy) { glyph_decode_row(font, glyph, iy, pixels[k]); keys[k] = iy; }
        if (keys[next] != iy + 1) { glyph_decode_row(font, glyph, iy + 1, pixels[next]); keys[next] = iy + 1; }
        uint16_t *row = out + (y - y0) * stride;
        for (int x = xa; x < xb; x++) {
            unsigned index = columns.first[x - xa];
            unsigned coverage = bilinear(pixels[k][index], pixels[k][index + 1], pixels[next][index], pixels[next][index + 1],
                                          columns.fraction[x - xa], fy);
            if (coverage) composite(&row[x - x0], color, opacity, coverage, swap);
        }
    }
}

static R_HOT void text_overlay(const scene_t *scene, const prim_t *p, uint16_t *out,
                         int x0, int y0, int width, int rows, int stride, bool swap) {
    const font_t *font = &g_fonts[p->r];
    int top = native_floor(p->sub_y - font->ascent - 2);
    int bottom = native_ceil(p->sub_y + font->descent + 2);
    if (top >= y0 + rows || bottom <= y0) return;
    int pen = p->sub_x * 16;
    for (int i = 0; i < p->b; i++) {
        unsigned index = scene->text[p->a + i];
        const font_glyph_t *glyph = &font->g[index];
        int gx = ((pen + 8) >> 4) + glyph->x, gy = p->sub_y - glyph->top;
        glyph_overlay(font, glyph, out, x0, y0, width, rows, stride, gx, gy, p->color, p->alpha, swap);
        pen += glyph->adv + p->sub_r + (i + 1 < p->b ? font_kern(font, index, scene->text[p->a + i + 1]) : 0);
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

static R_HOT void qr_overlay(const scene_t *scene, const prim_t *p, uint16_t *out,
                       int x0, int y0, int width, int rows, int stride, bool swap) {
    int ox = p->sub_x, oy = p->sub_y, side = p->b;
    if (side <= 0 || !scene->qr) return;
    int xa = maximum(maximum(x0, 0), ox), xb = minimum(minimum(x0 + width, UI_OUTPUT_W), ox + side);
    int ya = maximum(maximum(y0, 0), oy), yb = minimum(minimum(y0 + rows, UI_OUTPUT_H), oy + side);
    unsigned alpha = (p->alpha * 32u + 127u) / 255u;
    for (int y = ya; y < yb; y++) {
        uint16_t *row = out + (y - y0) * stride;
        int source = ((y - oy) * p->a / side) * p->a;
        for (int x = xa; x < xb; x++)
            if (scene->qr[source + (x - ox) * p->a / side]) edge_composite(&row[x - x0], p->color, alpha, swap);
    }
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
