// Native font source coordinates, transparent support and stride stay exact.
#include "../main/render_overlay_s3.c"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static unsigned source_pixel(const font_t *font, const font_glyph_t *glyph, int x, int y) {
    if (x < 0 || y < 0 || x >= glyph->w || y >= glyph->h) return 0;
    unsigned byte = font->px[glyph->off + y * ((glyph->w + 1) / 2) + x / 2];
    return (byte >> ((x & 1) * 4)) & 15;
}
static uint16_t reference(const font_t *font, const font_glyph_t *glyph, int x, int y, int gx, int gy,
                          uint8_t opacity, bool swap) {
    uint16_t bg = 0x3277, fg = 0xE593;
    if (x < 0 || y < 0 || x >= UI_OUTPUT_W || y >= UI_OUTPUT_H) return swap ? rgb565_bswap(bg) : bg;
    double sx = floor((((x + .5) * 30 / 23) - .5) * 256) / 256 - gx;
    double sy = floor((((y + .5) * 30 / 23) - .5) * 256) / 256 - gy;
    int ix = (int)floor(sx), iy = (int)floor(sy); double fx = sx - ix, fy = sy - iy;
    double coverage = source_pixel(font, glyph, ix, iy) * (1 - fx) * (1 - fy)
        + source_pixel(font, glyph, ix + 1, iy) * fx * (1 - fy)
        + source_pixel(font, glyph, ix, iy + 1) * (1 - fx) * fy
        + source_pixel(font, glyph, ix + 1, iy + 1) * fx * fy;
    unsigned q8 = (unsigned)floor(coverage * 256 + .5);
    unsigned alpha = (q8 * opacity * 32 + 489600) / 979200;
    uint16_t color = alpha >= 32 ? fg : rgb565_blend(fg, bg, alpha);
    return swap ? rgb565_bswap(color) : color;
}
static void check_case(const font_t *font, const font_glyph_t *glyph, int gx, int gy, int x0, int y0,
                       int width, int rows, uint8_t opacity, bool swap) {
    static uint16_t guarded[8 + (UI_OUTPUT_W + 16) * 80 + 8];
    int stride = width + 16;
    for (int i = 0; i < 8 + stride * rows + 8; i++) guarded[i] = 0xA55A;
    for (int y = 0; y < rows; y++) for (int x = 0; x < width; x++)
        guarded[8 + y * stride + x] = swap ? rgb565_bswap(0x3277) : 0x3277;
    glyph_overlay(font, glyph, guarded + 8, x0, y0, width, rows, stride, gx, gy, 0xE593, opacity, swap);
    for (int y = 0; y < rows; y++) for (int x = 0; x < stride; x++) {
        uint16_t want = x < width ? reference(font, glyph, x0 + x, y0 + y, gx, gy, opacity, swap) : 0xA55A;
        assert(guarded[8 + y * stride + x] == want);
    }
    for (int i = 0; i < 8; i++) {
        assert(guarded[i] == 0xA55A);
        assert(guarded[8 + rows * stride + i] == 0xA55A);
    }
}
static void real_glyphs(void) {
    static const uint8_t opacities[] = {0, 1, 193, 255}; unsigned checked = 0;
    for (int f = 0; f < FONT_COUNT; f++) for (int g = 0; g < g_fonts[f].n; g++)
        for (unsigned a = 0; a < sizeof opacities; a++) for (int swap = 0; swap < 2; swap++) {
            const font_t *font = &g_fonts[f]; const font_glyph_t *glyph = &font->g[g];
            check_case(font, glyph, -2, -2, -4, -4, 80, 80, opacities[a], swap);
            check_case(font, glyph, 100, 100, 70, 70, 80, 80, opacities[a], swap);
            check_case(font, glyph, 473, 578, 352, 432, 16, 16, opacities[a], swap);
            checked += 3;
        }
    printf("native glyph: %u actual-font windows match independent Q8/opacity oracle including negative origins and strided edges\n", checked);
}
static void maximum_width(void) {
    uint8_t bitmap[128 * 7]; for (unsigned i = 0; i < sizeof bitmap; i++) bitmap[i] = (uint8_t)(i * 37);
    font_t font = {.px = bitmap}; font_glyph_t glyph = {.w = UINT8_MAX, .h = 7};
    for (int gx = 0; gx < 30; gx++) check_case(&font, &glyph, gx, -1, 0, 0, UI_OUTPUT_W, 16, 255, false);
    puts("native glyph: maximum uint8 source width fits bounded column/row scratch at every scale phase");
}
int main(void) { real_glyphs(); maximum_width(); return 0; }
