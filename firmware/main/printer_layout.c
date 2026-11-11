#include "printer_layout_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool dimension(double value, double minimum, double maximum) {
    return isfinite(value) && (value == 0 || (value >= minimum && value <= maximum));
}

static bool unicode_space(uint32_t cp) {
    return cp == ' ' || cp == '\n' || cp == '\t' || cp == 0xa0 || cp == 0x1680 ||
        (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
        cp == 0x202f || cp == 0x205f || cp == 0x3000 || cp == 0xfeff;
}

static bool valid_text(const char *text, size_t *length) {
    if (!text) return false;
    *length = strnlen(text, PRINTER_LAYOUT_TEXT_MAX_BYTES + 1);
    if (!*length || *length > PRINTER_LAYOUT_TEXT_MAX_BYTES) return false;
    const char *p = text, *end = p + *length;
    bool visible = false;
    while (p < end) {
        uint32_t cp;
        if (!printer_utf8_read(&p, end, &cp)) return false;
        if ((cp < 0x20 && cp != '\n' && cp != '\t') || (cp >= 0x7f && cp <= 0x9f)) return false;
        visible |= !unicode_space(cp);
    }
    return visible;
}

static bool valid_document(const printer_document_t *doc, size_t *length) {
    if (!doc) return false;
    if (doc->format != PRINTER_FORMAT_TEXT && doc->format != PRINTER_FORMAT_QR) return false;
    if (!dimension(doc->width_mm, 1, 216) || !dimension(doc->height_mm, 1, PRINTER_LAYOUT_MAX_HEIGHT_MM) ||
        !dimension(doc->font_mm, 1.5, 8)) return false;
    if (!valid_text(doc->text, length)) return false;
    if (!printer_emoji_valid(doc->emojis, doc->emoji_count)) return false;
    if (doc->format == PRINTER_FORMAT_QR &&
        (doc->font_mm != 0 || doc->emoji_count || *length > PRINTER_LAYOUT_QR_MAX_BYTES)) return false;
    return true;
}

bool printer_document_valid(const printer_document_t *document) {
    size_t length;
    return valid_document(document, &length);
}

static bool set_box(printer_layout_t *layout, const printer_document_t *doc, unsigned dpi, unsigned feed_dpi, unsigned head) {
    int width = doc->width_mm ? (int)lround(doc->width_mm * dpi / 25.4) : (int)head;
    if (width < 1 || width > (int)head) return false;
    layout->info = (printer_layout_info_t){.format = doc->format, .dpi = dpi, .feed_dpi = feed_dpi,
        .head_width_dots = head, .width_dots = width};
    if (doc->height_mm) {
        unsigned height = (unsigned)lround(doc->height_mm * feed_dpi / 25.4);
        unsigned limit = (unsigned)floor(PRINTER_LAYOUT_MAX_HEIGHT_MM * feed_dpi / 25.4);
        layout->info.height_dots = height < limit ? height : limit;
    }
    else if (doc->format == PRINTER_FORMAT_QR) {
        unsigned limit = (unsigned)floor(PRINTER_LAYOUT_MAX_HEIGHT_MM * feed_dpi / 25.4);
        unsigned height = (unsigned)lround(width * (double)feed_dpi / dpi);
        layout->info.height_dots = height < limit ? height : limit;
    }
    return true;
}

static bool valid_profile(unsigned dpi, unsigned feed_dpi, unsigned head) {
    return dpi >= 72 && dpi <= 600 && feed_dpi >= 72 && feed_dpi <= 600 &&
        head >= 8 && head <= 2048 && head % 8 == 0;
}

printer_layout_t *printer_layout_create(const printer_document_t *doc,
    unsigned dpi, unsigned feed_dpi, unsigned head, printer_layout_error_t *error) {
    printer_layout_error_t local;
    if (!error) error = &local;
    *error = PRINTER_LAYOUT_INVALID_REQUEST;
    size_t length;
    if (!valid_profile(dpi, feed_dpi, head) || !valid_document(doc, &length)) return NULL;
    printer_layout_t *layout = calloc(1, sizeof *layout);
    if (!layout) { *error = PRINTER_LAYOUT_NO_MEMORY; return NULL; }
    if (!set_box(layout, doc, dpi, feed_dpi, head)) { free(layout); return NULL; }
    memcpy(layout->text, doc->text, length + 1);
    layout->text_bytes = length; layout->emojis = doc->emojis; layout->emoji_count = doc->emoji_count;
    bool ok = doc->format == PRINTER_FORMAT_TEXT ? printer_layout_text(layout, doc, error) : printer_layout_qr(layout, error);
    if (!ok) { free(layout); return NULL; }
    layout->info.width_mm = layout->info.width_dots * 25.4 / dpi;
    layout->info.height_mm = layout->info.height_dots * 25.4 / feed_dpi;
    *error = PRINTER_LAYOUT_OK;
    return layout;
}

void printer_layout_destroy(printer_layout_t *layout) { free(layout); }
const printer_layout_info_t *printer_layout_info(const printer_layout_t *layout) { return layout ? &layout->info : NULL; }

const char *printer_layout_error_code(printer_layout_error_t error) {
    static const char *const codes[] = {NULL, "invalid_request", "content_does_not_fit", "unsupported_character", "printer_failed"};
    return error >= PRINTER_LAYOUT_OK && error <= PRINTER_LAYOUT_NO_MEMORY ? codes[error] : "printer_failed";
}

bool printer_layout_row(const printer_layout_t *layout, unsigned y, uint8_t *row, size_t row_bytes) {
    if (!layout || !row || y >= layout->info.height_dots || row_bytes != (layout->info.head_width_dots + 7u) / 8) return false;
    memset(row, 0, row_bytes);
    if (layout->info.format == PRINTER_FORMAT_TEXT) printer_text_row(layout, y, row);
    else printer_qr_row(layout, y, row);
    return true;
}
