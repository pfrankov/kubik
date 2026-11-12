#include "printer_layout_internal.h"

#include <stdlib.h>

static unsigned edge(unsigned index, unsigned span, unsigned modules) {
    return (index * span + modules / 2) / modules;
}

static unsigned module_at(unsigned position, unsigned span, unsigned modules) {
    return ((position + 1) * modules - 1 - modules / 2) / span;
}

bool printer_layout_qr(printer_layout_t *layout, printer_layout_error_t *error) {
    uint8_t *temporary = malloc(PRINTER_QR_BUFFER);
    if (!temporary) { *error = PRINTER_LAYOUT_NO_MEMORY; return false; }
    bool encoded = qrcodegen_encodeText(layout->text, temporary, layout->payload.qr, qrcodegen_Ecc_MEDIUM,
        1, PRINTER_QR_VERSION, qrcodegen_Mask_AUTO, false);
    free(temporary);
    if (!encoded) { *error = PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT; return false; }
    unsigned size = qrcodegen_getSize(layout->payload.qr), outer = size + 8;
    unsigned width = layout->info.width_dots, height = layout->info.height_dots;
    unsigned qr_width = width;
    unsigned qr_height = (width * layout->info.feed_dpi + layout->info.dpi / 2) / layout->info.dpi;
    if (qr_height > height) {
        qr_height = height;
        qr_width = (height * layout->info.dpi + layout->info.feed_dpi / 2) / layout->info.feed_dpi;
        if (qr_width > width) qr_width = width;
    }
    if (qr_width / outer < 3 || qr_height / outer < 2) {
        *error = PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT; return false;
    }
    layout->qr_size = size;
    layout->qr_width = qr_width;
    layout->qr_height = qr_height;
    layout->qr_x = (width - qr_width) / 2;
    layout->qr_y = (height - qr_height) / 2;
    return true;
}

void printer_qr_row(const printer_layout_t *layout, unsigned y, uint8_t *row) {
    if (y < layout->qr_y || y - layout->qr_y >= layout->qr_height) return;
    unsigned outer = layout->qr_size + 8;
    int my = (int)module_at(y - layout->qr_y, layout->qr_height, outer) - 4;
    if (my < 0 || my >= layout->qr_size) return;
    for (unsigned mx = 0; mx < layout->qr_size; ++mx) {
        if (!qrcodegen_getModule(layout->payload.qr, mx, my)) continue;
        unsigned left = layout->qr_x + edge(mx + 4, layout->qr_width, outer);
        unsigned right = layout->qr_x + edge(mx + 5, layout->qr_width, outer);
        for (unsigned x = left; x < right; ++x) printer_black(row, x);
    }
}
