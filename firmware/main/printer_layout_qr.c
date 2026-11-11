#include "printer_layout_internal.h"

#include <stdlib.h>

static unsigned divisor(unsigned a, unsigned b) {
    while (b) { unsigned next = a % b; a = b; b = next; }
    return a;
}

bool printer_layout_qr(printer_layout_t *layout, printer_layout_error_t *error) {
    uint8_t *temporary = malloc(PRINTER_QR_BUFFER);
    if (!temporary) { *error = PRINTER_LAYOUT_NO_MEMORY; return false; }
    bool encoded = qrcodegen_encodeText(layout->text, temporary, layout->payload.qr, qrcodegen_Ecc_MEDIUM,
        1, PRINTER_QR_VERSION, qrcodegen_Mask_AUTO, false);
    free(temporary);
    if (!encoded) { *error = PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT; return false; }
    unsigned size = qrcodegen_getSize(layout->payload.qr), outer = size + 8;
    unsigned gcd = divisor(layout->info.dpi, layout->info.feed_dpi);
    unsigned unit_x = layout->info.dpi / gcd, unit_y = layout->info.feed_dpi / gcd;
    unsigned scale_x = layout->info.width_dots / (outer * unit_x);
    unsigned scale_y = layout->info.height_dots / (outer * unit_y);
    unsigned scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale * unit_x < 3 || scale * unit_y < 2) { *error = PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT; return false; }
    layout->qr_size = size;
    layout->qr_scale_x = scale * unit_x;
    layout->qr_scale_y = scale * unit_y;
    layout->qr_x = (layout->info.width_dots - outer * layout->qr_scale_x) / 2;
    layout->qr_y = (layout->info.height_dots - outer * layout->qr_scale_y) / 2;
    return true;
}

void printer_qr_row(const printer_layout_t *layout, unsigned y, uint8_t *row) {
    int my = ((int)y - layout->qr_y) / layout->qr_scale_y - 4;
    if (y < layout->qr_y || my < 0 || my >= layout->qr_size) return;
    for (unsigned mx = 0; mx < layout->qr_size; ++mx) {
        if (!qrcodegen_getModule(layout->payload.qr, mx, my)) continue;
        unsigned left = layout->qr_x + (mx + 4) * layout->qr_scale_x;
        for (unsigned x = left; x < left + layout->qr_scale_x; ++x) printer_black(row, x);
    }
}
