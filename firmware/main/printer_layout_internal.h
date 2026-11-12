#pragma once

#include "printer_layout.h"
#include "font.h"
#include "qrcodegen.h"

#define PRINTER_LAYOUT_MAX_LINES 128
#define PRINTER_QR_VERSION 12
#define PRINTER_QR_BUFFER qrcodegen_BUFFER_LEN_FOR_VERSION(PRINTER_QR_VERSION)

typedef struct { uint16_t start, end; int16_t shift; } printer_line_t;
struct printer_layout {
    printer_layout_info_t info;
    char text[PRINTER_LAYOUT_TEXT_MAX_BYTES + 1];
    const font_t *font;
    const printer_emoji_glyph_t *emojis;
    unsigned emoji_count;
    uint16_t text_bytes, emoji_w, emoji_h;
    bool has_emoji;
    uint16_t cap_x, cap_y, pad_x, pad_y, ascent, descent, line_step;
    union {
        printer_line_t lines[PRINTER_LAYOUT_MAX_LINES];
        uint8_t qr[PRINTER_QR_BUFFER];
    } payload;
    uint16_t qr_size, qr_width, qr_height, qr_x, qr_y;
};

bool printer_layout_text(printer_layout_t *layout, const printer_document_t *document,
    printer_layout_error_t *error);
bool printer_layout_qr(printer_layout_t *layout, printer_layout_error_t *error);
void printer_text_row(const printer_layout_t *layout, unsigned y, uint8_t *row);
void printer_qr_row(const printer_layout_t *layout, unsigned y, uint8_t *row);

static inline int printer_scaled_floor(int value, int cap, int base) {
    int numerator = value * cap;
    return numerator >= 0 ? numerator / base : -((-numerator + base - 1) / base);
}
static inline int printer_scaled_ceil(int value, int cap, int base) {
    return -printer_scaled_floor(-value, cap, base);
}
static inline void printer_black(uint8_t *row, unsigned x) { row[x / 8] |= 0x80u >> (x % 8); }
