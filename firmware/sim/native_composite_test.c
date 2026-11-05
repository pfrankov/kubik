// Exercise the exact native alpha hot helper at every supported coverage/opacity.
#include "../main/render_overlay_s3.c"
#include <assert.h>
#include <stdio.h>

static void check_pixel(unsigned coverage, unsigned opacity, bool swap) {
    static const uint16_t colors[] = {0, 0xFFFF, 0x3277, 0xE593};
    uint16_t fg = colors[(coverage + opacity) % 4], bg = colors[(coverage * 3 + opacity / 4) % 4];
    unsigned alpha = (coverage * opacity * 32u + 489600u) / 979200u;
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
int main(void) {
    for (unsigned coverage = 0; coverage <= 15 * 256; coverage++)
        for (unsigned opacity = 0; opacity <= 255; opacity++) {
            check_pixel(coverage, opacity, false);
            check_pixel(coverage, opacity, true);
        }
    bilinear_exact();
    puts("native composite: 1966592 exact coverage/opacity/wire-order cases match the original integer formula");
    return 0;
}
