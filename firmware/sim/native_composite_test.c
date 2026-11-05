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
int main(void) {
    for (unsigned coverage = 0; coverage <= 15 * 256; coverage++)
        for (unsigned opacity = 0; opacity <= 255; opacity++) {
            check_pixel(coverage, opacity, false);
            check_pixel(coverage, opacity, true);
        }
    puts("native composite: 1966592 exact coverage/opacity/wire-order cases match the original integer formula");
    return 0;
}
