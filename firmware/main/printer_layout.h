#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "printer_emoji.h"

#define PRINTER_LAYOUT_TEXT_MAX_BYTES 1024
#define PRINTER_LAYOUT_QR_MAX_BYTES 256
#define PRINTER_LAYOUT_MAX_HEIGHT_MM 150.0

typedef enum { PRINTER_FORMAT_TEXT, PRINTER_FORMAT_QR } printer_format_t;
typedef enum {
    PRINTER_LAYOUT_OK,
    PRINTER_LAYOUT_INVALID_REQUEST,
    PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT,
    PRINTER_LAYOUT_UNSUPPORTED_CHARACTER,
    PRINTER_LAYOUT_NO_MEMORY,
} printer_layout_error_t;

typedef struct {
    const char *text;
    printer_format_t format;
    // Zero means automatic/default, including font size (2.5 mm capitals).
    double width_mm, height_mm, font_mm;
    const printer_emoji_glyph_t *emojis;
    unsigned emoji_count;
} printer_document_t;

typedef struct {
    printer_format_t format;
    // Head dots and feed rows have independent physical pitch.
    uint16_t width_dots, height_dots, dpi, feed_dpi, head_width_dots;
    double width_mm, height_mm, font_mm;
    const printer_emoji_glyph_t *emojis;
    unsigned emoji_count;
    unsigned lines;
} printer_layout_info_t;

typedef struct printer_layout printer_layout_t;

// Bounded field/UTF-8/control validation only; no allocation, glyph lookup or UI.
bool printer_document_valid(const printer_document_t *document);
// Copies the bounded text. Glyphs are borrowed and must remain immutable until
// destroy (the printer job owns them). No full-page bitmap or UI state is retained.
printer_layout_t *printer_layout_create(const printer_document_t *document,
    unsigned dpi, unsigned feed_dpi, unsigned head_width_dots, printer_layout_error_t *error);
void printer_layout_destroy(printer_layout_t *layout);
const printer_layout_info_t *printer_layout_info(const printer_layout_t *layout);
const char *printer_layout_error_code(printer_layout_error_t error);

// One full-head row, MSB first, with unused box/head pixels white. Exact row
// length ceil(head_width/8) is required. Invalid arguments leave output intact.
bool printer_layout_row(const printer_layout_t *layout, unsigned y,
    uint8_t *row, size_t row_bytes);
