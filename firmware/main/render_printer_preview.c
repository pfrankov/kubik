#include "render_printer_preview.h"

void render_printer_preview(const scene_t *scene, const prim_t *prim, uint16_t *buffer,
                            int x0, int width, int y0, int y1, int band_y0) {
    const uint8_t *data = printer_preview_pixels(scene->printer_preview);
    if (!data) return;
    int xs = prim->bx0 > x0 ? prim->bx0 : x0;
    int xe = prim->bx1 < x0 + width - 1 ? prim->bx1 : x0 + width - 1;
    int w = prim->bx1 - prim->bx0 + 1, h = prim->by1 - prim->by0 + 1;
    if (w <= 0 || h <= 0) return;
    for (int y = y0; y <= y1; y++) {
        int sy = (y - prim->by0) * PRINTER_PREVIEW_EDGE / h;
        uint16_t *row = buffer + (y - band_y0) * width;
        for (int x = xs; x <= xe; x++) {
            int sx = (x - prim->bx0) * PRINTER_PREVIEW_EDGE / w, index = sy * PRINTER_PREVIEW_EDGE + sx;
            uint8_t pair = data[32 + index / 2];
            unsigned color = index & 1 ? pair & 15 : pair >> 4;
            uint16_t value = ((uint16_t)data[color * 2] << 8) | data[color * 2 + 1];
            row[x - x0] = g_render_swap ? rgb565_bswap(value) : value;
        }
    }
}
