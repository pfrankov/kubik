// Exercise the exact native alpha hot helper at every supported coverage/opacity.
#include "../main/render_overlay_s3.c"
#include <assert.h>
#include <stdio.h>

static void check_pixel(unsigned coverage, unsigned opacity, bool swap) {
    static const uint16_t colors[] = {0, 0xFFFF, 0x3277, 0xE593};
    uint16_t fg = colors[(coverage + opacity) % 4], bg = colors[(coverage * 3 + opacity / 4) % 4];
    unsigned alpha = (coverage * opacity * 32u + 489600u) / 979200u;
    assert(coverage_alpha(coverage, (uint8_t)opacity) == alpha);
    uint16_t want = alpha >= 32 ? fg : rgb565_blend(fg, bg, alpha);
    uint16_t pixel = swap ? rgb565_bswap(bg) : bg;
    composite(&pixel, fg, (uint8_t)opacity, coverage, swap);
    assert(pixel == (swap ? rgb565_bswap(want) : want));
}
static void bilinear_exact(void) {
    for (unsigned pattern = 0; pattern < 16; pattern++) {
        unsigned a = pattern & 1 ? 15 : 0, b = pattern & 2 ? 15 : 0;
        unsigned c = pattern & 4 ? 15 : 0, d = pattern & 8 ? 15 : 0;
        for (int fx = 0; fx < 256; fx++) for (int fy = 0; fy < 256; fy++) {
            unsigned top = a * (256 - fx) + b * fx, bottom = c * (256 - fx) + d * fx;
            unsigned want = (top * (256 - fy) + bottom * fy + 128) >> 8;
            assert(bilinear(a, b, c, d, fx, fy) == want);
        }
    }
    puts("native bilinear: 1048576 extreme-edge/fraction cases preserve the original Q8 rounding");
}
static void check_qr_span(unsigned alpha, uint16_t color, bool swap, int offset, int count, int pattern) {
    _Alignas(4) uint16_t pixels[24], want[24];
    for (int i = 0; i < 24; i++) pixels[i] = want[i] = 0xA55A;
    for (int i = 0; i < count; i++) {
        uint16_t bg = pattern == 0 ? 0 : pattern == 1 && !(i & 1) ? 0 : (uint16_t)(0x3277 + i * 0x329);
        pixels[2 + offset + i] = swap ? rgb565_bswap(bg) : bg;
        uint16_t expected = alpha >= 32 ? color : rgb565_blend(color, bg, alpha);
        want[2 + offset + i] = swap ? rgb565_bswap(expected) : expected;
    }
    uint16_t black = rgb565_blend(color, 0, alpha);
    qr_brush_t brush = {.color = color, .wire = swap ? rgb565_bswap(color) : color,
        .black = swap ? rgb565_bswap(black) : black, .alpha = (uint8_t)alpha, .swap = swap};
    qr_paint_span(&brush, pixels + 2 + offset, count);
    for (int i = 0; i < 24; i++) assert(pixels[i] == want[i]);
}
static void qr_pairs_exact(void) {
    static const uint16_t colors[] = {0, 0xFFFF, 0xE593};
    for (unsigned alpha = 0; alpha <= 32; alpha++) for (unsigned c = 0; c < 3; c++)
        for (int swap = 0; swap < 2; swap++) for (int offset = 0; offset < 2; offset++)
            for (int count = 0; count < 18; count++) for (int pattern = 0; pattern < 3; pattern++)
                check_qr_span(alpha, colors[c], swap, offset, count, pattern);
    puts("native QR spans:21384 pair/odd/zero/mixed-background cases preserve exact scalar pixels and canaries");
}
static void design_coordinates(void) {
    // The real clipped panel uses0..447; the reciprocal remains exact until
    // its explicitly uint16_t numerator reaches65535.
    for (int native = 0; native <= 32756; native++)
        assert(design_q8(native) == ((native * 2 + 1) * 30 * 128) / 23 - 128);
    puts("native design axis: clipped0..447 and extended0..32756 coordinates match the original division exactly");
}
int main(void) {
    for (unsigned coverage = 0; coverage <= 15 * 256; coverage++)
        for (unsigned opacity = 0; opacity <= 255; opacity++) {
            check_pixel(coverage, opacity, false);
            check_pixel(coverage, opacity, true);
        }
    design_coordinates();
    bilinear_exact();
    qr_pairs_exact();
    puts("native composite: 1966592 exact coverage/opacity/wire-order cases match the original integer formula");
    return 0;
}
