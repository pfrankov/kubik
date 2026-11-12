#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "printer_layout.h"
#include "printer_layout_internal.h"
#include "font.h"

typedef union { struct { size_t bytes; } value; max_align_t alignment; } allocation_t;
static size_t live_bytes, peak_bytes;
static unsigned allocations, fail_at;

void *printer_test_malloc(size_t bytes) {
    if (++allocations == fail_at) return NULL;
    allocation_t *p = malloc(sizeof(*p) + bytes);
    assert(p);
    p->value.bytes = bytes;
    live_bytes += bytes;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return p + 1;
}
void *printer_test_calloc(size_t n, size_t bytes) {
    assert(!bytes || n <= SIZE_MAX / bytes);
    void *p = printer_test_malloc(n * bytes);
    if (p) memset(p, 0, n * bytes);
    return p;
}
void printer_test_free(void *data) {
    if (!data) return;
    allocation_t *p = (allocation_t *)data - 1;
    assert(live_bytes >= p->value.bytes);
    live_bytes -= p->value.bytes;
    free(p);
}

static printer_layout_t *make(printer_document_t doc, unsigned dpi, unsigned width) {
    printer_layout_error_t error = PRINTER_LAYOUT_NO_MEMORY;
    printer_layout_t *layout = printer_layout_create(&doc, dpi, dpi, width, &error);
    assert(layout && error == PRINTER_LAYOUT_OK);
    return layout;
}
static printer_layout_t *make_axes(printer_document_t doc, unsigned dpi, unsigned feed_dpi) {
    printer_layout_error_t error = PRINTER_LAYOUT_NO_MEMORY;
    printer_layout_t *layout = printer_layout_create(&doc, dpi, feed_dpi, 576, &error);
    assert(layout && error == PRINTER_LAYOUT_OK);
    return layout;
}
static void rejected(printer_document_t doc, printer_layout_error_t expected) {
    printer_layout_error_t error = PRINTER_LAYOUT_OK;
    printer_layout_t *layout = printer_layout_create(&doc, 304, 152, 576, &error);
    assert(!layout && error == expected && live_bytes == 0);
}
static bool dark(const uint8_t *row, unsigned x) { return (row[x / 8] & (0x80u >> (x % 8))) != 0; }

typedef struct { unsigned left, top, right, bottom, count; uint32_t hash; } ink_t;
static ink_t inspect(const printer_layout_t *layout) {
    const printer_layout_info_t *info = printer_layout_info(layout);
    ink_t ink = {.left = info->head_width_dots, .top = info->height_dots, .hash = 2166136261u};
    uint8_t row[256 + 2];
    size_t bytes = info->head_width_dots / 8;
    for (unsigned y = 0; y < info->height_dots; ++y) {
        memset(row, 0xa5, sizeof row);
        assert(printer_layout_row(layout, y, row + 1, bytes));
        assert(row[0] == 0xa5 && row[bytes + 1] == 0xa5);
        for (unsigned x = 0; x < info->head_width_dots; ++x) {
            ink.hash = (ink.hash ^ dark(row + 1, x)) * 16777619u;
            if (!dark(row + 1, x)) continue;
            assert(x < info->width_dots);
            if (x < ink.left) ink.left = x;
            if (x > ink.right) ink.right = x;
            if (y < ink.top) ink.top = y;
            if (y > ink.bottom) ink.bottom = y;
            ++ink.count;
        }
    }
    assert(ink.count);
    return ink;
}

static void complete_text(const printer_layout_t *layout) {
    char expected[1025] = {0}, actual[1025] = {0};
    unsigned a = 0, b = 0;
    for (const char *p = layout->text; *p; ++p)
        if (*p != ' ' && *p != '\t' && *p != '\n') expected[a++] = *p;
    for (unsigned i = 0; i < layout->info.lines; ++i) {
        const printer_line_t *line = &layout->payload.lines[i];
        for (unsigned j = line->start; j < line->end; ++j)
            if (layout->text[j] != ' ' && layout->text[j] != '\t') actual[b++] = layout->text[j];
    }
    assert(a == b && !memcmp(expected, actual, a));
}

static void text_sizes(void) {
    printer_document_t doc = {.text = "Привет, Кубик!\nЁж и ёлка\nHello", .width_mm = 32};
    printer_layout_t *layout = make(doc, 304, 576);
    const printer_layout_info_t *info = printer_layout_info(layout);
    assert(info->dpi == 304 && info->width_dots == 383 && info->lines >= 3);
    assert(fabs(info->width_mm - info->width_dots * 25.4 / 304) < 1e-9);
    assert(info->font_mm >= 1.5 && info->font_mm <= 2.55 && info->height_mm <= 150);
    complete_text(layout);
    ink_t ink = inspect(layout);
    unsigned padding = (unsigned)lround(304 / 25.4);
    assert(ink.left >= padding && ink.right < info->width_dots - padding);
    assert(ink.top >= padding && ink.bottom < info->height_dots - padding);
    assert(inspect(layout).hash == ink.hash);
    printer_layout_destroy(layout);
    assert(live_bytes == 0);
}

static void wraps_and_height(void) {
    printer_layout_t *layout = make((printer_document_t){.text = "Alpha beta", .width_mm = 16}, 304, 576);
    assert(printer_layout_info(layout)->lines == 2);
    complete_text(layout);
    inspect(layout);
    printer_layout_destroy(layout);
    layout = make((printer_document_t){.text = "Supercalifragilistic", .width_mm = 10}, 304, 576);
    assert(printer_layout_info(layout)->lines > 2);
    complete_text(layout);
    inspect(layout);
    printer_layout_destroy(layout);
    layout = make((printer_document_t){.text = "A\n\nB\n", .width_mm = 20}, 304, 576);
    assert(printer_layout_info(layout)->lines == 4);
    printer_layout_destroy(layout);
    layout = make((printer_document_t){.text = "A\nB", .width_mm = 20, .height_mm = 12, .font_mm = 6}, 304, 576);
    const printer_layout_info_t *info = printer_layout_info(layout);
    assert(info->height_dots == 144 && info->font_mm < 6 && info->font_mm >= 1.5 && info->lines == 2);
    inspect(layout);
    printer_layout_destroy(layout);
    rejected((printer_document_t){.text = "A", .height_mm = 1}, PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT);
    rejected((printer_document_t){.text = "A", .width_mm = 1}, PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT);
    char maximum[1025]; memset(maximum, 'w', 1024); maximum[1024] = 0;
    layout = make((printer_document_t){.text = maximum}, 304, 576);
    complete_text(layout); inspect(layout); printer_layout_destroy(layout);
    rejected((printer_document_t){.text = maximum, .font_mm = 8}, PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT);
}

static void input_boundaries(void) {
    unsigned before = allocations;
    assert(printer_document_valid(&(printer_document_t){.text = "valid UTF-8: Привет"}));
    assert(!printer_document_valid(&(printer_document_t){.text = "invalid\x1b"}));
    assert(!printer_document_valid(NULL) && allocations == before);
    rejected((printer_document_t){.text = "  \t\n"}, PRINTER_LAYOUT_INVALID_REQUEST);
    rejected((printer_document_t){.text = "A\x1b@"}, PRINTER_LAYOUT_INVALID_REQUEST);
    rejected((printer_document_t){.text = "A\x7f"}, PRINTER_LAYOUT_INVALID_REQUEST);
    rejected((printer_document_t){.text = "\xc0\xaf"}, PRINTER_LAYOUT_INVALID_REQUEST);
    rejected((printer_document_t){.text = "\xe2\x82"}, PRINTER_LAYOUT_INVALID_REQUEST);
    rejected((printer_document_t){.text = "\xed\xa0\x80"}, PRINTER_LAYOUT_INVALID_REQUEST);
    rejected((printer_document_t){.text = "Hello 😀"}, PRINTER_LAYOUT_UNSUPPORTED_CHARACTER);
    rejected((printer_document_t){.text = "A", .width_mm = NAN}, PRINTER_LAYOUT_INVALID_REQUEST);
    rejected((printer_document_t){.text = "A", .height_mm = INFINITY}, PRINTER_LAYOUT_INVALID_REQUEST);
    rejected((printer_document_t){.text = "A", .font_mm = 1.4}, PRINTER_LAYOUT_INVALID_REQUEST);
    rejected((printer_document_t){.text = "A", .format = 9}, PRINTER_LAYOUT_INVALID_REQUEST);
    rejected((printer_document_t){.text = "A", .width_mm = 576 * 25.4 / 304 + .05}, PRINTER_LAYOUT_INVALID_REQUEST);
    char excess[1026]; memset(excess, 'a', sizeof excess); excess[1025] = 0;
    rejected((printer_document_t){.text = excess}, PRINTER_LAYOUT_INVALID_REQUEST);
}

static void ownership_and_rows(void) {
    char text[] = "A\tB";
    printer_layout_t *layout = make((printer_document_t){.text = text}, 203, 384);
    uint32_t before = inspect(layout).hash;
    text[0] = 'Z';
    assert(inspect(layout).hash == before);
    uint8_t row[72]; memset(row, 0x7b, sizeof row);
    assert(!printer_layout_row(layout, 0, row, 47));
    assert(!printer_layout_row(layout, printer_layout_info(layout)->height_dots, row, 48));
    assert(!printer_layout_row(NULL, 0, row, 48));
    for (unsigned i = 0; i < sizeof row; ++i) assert(row[i] == 0x7b);
    printer_layout_destroy(layout);
    assert(!printer_layout_info(NULL));
    printer_layout_destroy(NULL);
}

static void full_height_and_glyphs(void) {
    printer_document_t doc = {.text = "A", .height_mm = 150, .font_mm = 8};
    printer_layout_t *layout = make(doc, 203, 384);
    assert(printer_layout_info(layout)->height_mm <= 150 && printer_layout_info(layout)->font_mm <= 8);
    inspect(layout); printer_layout_destroy(layout);
    const font_t *font = &g_fonts[FONT_CARD];
    for (unsigned i = 0; i < font->n; ++i) {
        uint32_t cp = font->g[i].cp;
        if (!font->g[i].w || !font->g[i].h || cp == ' ' || cp == 0xa0) continue;
        char text[4] = {0};
        if (cp < 0x80) text[0] = (char)cp;
        else if (cp < 0x800) { text[0] = (char)(0xc0 | cp >> 6); text[1] = (char)(0x80 | (cp & 63)); }
        else { text[0] = (char)(0xe0 | cp >> 12); text[1] = (char)(0x80 | (cp >> 6 & 63)); text[2] = (char)(0x80 | (cp & 63)); }
        layout = make((printer_document_t){.text = text, .font_mm = 8}, 304, 576);
        ink_t ink = inspect(layout);
        const printer_layout_info_t *info = printer_layout_info(layout);
        assert(ink.left >= 12 && ink.right + 12u < info->width_dots && ink.top >= 12 && ink.bottom + 12u < info->height_dots);
        printer_layout_destroy(layout);
    }
}

static unsigned qr_edge(unsigned i, unsigned span, unsigned modules) {
    return (i * span + modules / 2) / modules;
}

static void qr_cell(const printer_layout_t *layout, unsigned mx, unsigned my) {
    unsigned outer = layout->qr_size + 8;
    unsigned x0 = layout->qr_x + qr_edge(mx, layout->qr_width, outer);
    unsigned x1 = layout->qr_x + qr_edge(mx + 1, layout->qr_width, outer);
    unsigned y0 = layout->qr_y + qr_edge(my, layout->qr_height, outer);
    unsigned y1 = layout->qr_y + qr_edge(my + 1, layout->qr_height, outer);
    assert(x1 - x0 >= 3 && y1 - y0 >= 2);
    bool inside = mx >= 4 && my >= 4 && mx < outer - 4 && my < outer - 4;
    bool expected = inside && qrcodegen_getModule(layout->payload.qr, (int)mx - 4, (int)my - 4);
    uint8_t row[72];
    for (unsigned y = y0; y < y1; ++y) {
        assert(printer_layout_row(layout, y, row, sizeof row));
        for (unsigned x = x0; x < x1; ++x) assert(dark(row, x) == expected);
    }
}

static void qr_geometry(printer_layout_t *layout) {
    const printer_layout_info_t *info = printer_layout_info(layout);
    inspect(layout);
    assert(layout->qr_x + layout->qr_width <= info->width_dots);
    assert(layout->qr_y + layout->qr_height <= info->height_dots);
    int aspect = (int)layout->qr_width * info->feed_dpi - (int)layout->qr_height * info->dpi;
    assert(abs(aspect) <= (int)(info->dpi > info->feed_dpi ? info->dpi : info->feed_dpi));
    unsigned outer = layout->qr_size + 8;
    for (unsigned y = 0; y < outer; ++y)
        for (unsigned x = 0; x < outer; ++x) qr_cell(layout, x, y);
    uint8_t row[72];
    for (unsigned y = 0; y < info->height_dots; ++y) {
        assert(printer_layout_row(layout, y, row, sizeof row));
        for (unsigned x = 0; x < info->head_width_dots; ++x)
            if (y < layout->qr_y || y >= layout->qr_y + layout->qr_height ||
                x < layout->qr_x || x >= layout->qr_x + layout->qr_width) assert(!dark(row, x));
    }
}

static void save_raster(const printer_layout_t *layout, const char *folder, const char *name) {
    if (!folder) return;
    char path[1024]; snprintf(path, sizeof path, "%s/%s.pbm", folder, name);
    FILE *file = fopen(path, "wb"); assert(file);
    const printer_layout_info_t *info = printer_layout_info(layout);
    fprintf(file, "P4\n# dpi %u %u\n%u %u\n", info->dpi, info->feed_dpi, info->head_width_dots, info->height_dots);
    uint8_t row[72];
    for (unsigned y = 0; y < info->height_dots; ++y) {
        assert(printer_layout_row(layout, y, row, sizeof row));
        assert(fwrite(row, 1, sizeof row, file) == sizeof row);
    }
    assert(!fclose(file));
}

static void axis_sizes(const char *folder) {
    printer_document_t doc = {.text = "MMMM\nПривет, Кубик!", .width_mm = 30, .height_mm = 20, .font_mm = 3};
    printer_layout_t *square = make_axes(doc, 304, 304);
    printer_layout_info_t a = *printer_layout_info(square);
    ink_t ink_a = inspect(square);
    unsigned square_cap_x = square->cap_x;
    assert(square->font == &g_fonts[FONT_CARD]);
    printer_layout_destroy(square);
    printer_layout_t *calibrated = make_axes(doc, 304, 152);
    const printer_layout_info_t *b = printer_layout_info(calibrated);
    assert(a.width_dots == b->width_dots && a.height_dots == 239 && b->height_dots == 120);
    assert(b->feed_dpi == 152 && b->font_mm == calibrated->cap_y * 25.4 / 152);
    assert(calibrated->cap_x == 2 * calibrated->cap_y);
    assert(calibrated->pad_x == 12 && calibrated->pad_y == 6);
    assert(calibrated->font == &g_fonts[FONT_CARD]);
    assert(a.lines == b->lines && b->font_mm >= 3 - 25.4 / 152);
    ink_t ink_b = inspect(calibrated);
    assert(abs((int)square_cap_x - (int)calibrated->cap_x) <= 2);
    double height_a = (ink_a.bottom - ink_a.top + 1) * 25.4 / a.feed_dpi;
    double height_b = (ink_b.bottom - ink_b.top + 1) * 25.4 / b->feed_dpi;
    assert(fabs(height_a - height_b) <= 2 * 25.4 / 152);
    assert(fabs(b->height_mm - b->height_dots * 25.4 / 152) < 1e-9);
    complete_text(calibrated); save_raster(calibrated, folder, "text-calibrated");
    printer_layout_destroy(calibrated);
    doc.text = "https://kubik.local/printer?size=20mm"; doc.format = PRINTER_FORMAT_QR;
    doc.width_mm = 20; doc.height_mm = doc.font_mm = 0;
    calibrated = make_axes(doc, 304, 152);
    b = printer_layout_info(calibrated);
    assert(b->width_dots == 239 && b->height_dots == 120);
    assert(abs((int)calibrated->qr_width - 2 * (int)calibrated->qr_height) <= 1);
    qr_geometry(calibrated); save_raster(calibrated, folder, "qr-calibrated-20mm");
    printer_layout_destroy(calibrated);
    calibrated = make_axes(doc, 304, 225);
    b = printer_layout_info(calibrated);
    assert(b->width_dots == 239 && b->height_dots == 177);
    qr_geometry(calibrated); save_raster(calibrated, folder, "qr-225-20mm");
    printer_layout_destroy(calibrated);
    doc.width_mm = 32; doc.height_mm = 22;
    calibrated = make_axes(doc, 304, 225);
    assert(calibrated->info.width_dots == 383 && calibrated->info.height_dots == 195);
    assert(calibrated->qr_width == 263 && calibrated->qr_height == 195);
    qr_geometry(calibrated); save_raster(calibrated, folder, "qr-225-rectangle");
    printer_layout_destroy(calibrated);
    printer_layout_error_t too_small;
    doc.width_mm = 5; doc.height_mm = 0;
    assert(!printer_layout_create(&doc, 304, 225, 576, &too_small));
    assert(too_small == PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT);
    calibrated = make_axes((printer_document_t){.text="A", .height_mm=150, .font_mm=8}, 304, 152);
    assert(calibrated->info.height_dots == 897 && calibrated->info.height_mm <= 150);
    inspect(calibrated); printer_layout_destroy(calibrated);
    printer_layout_error_t error;
    assert(!printer_layout_create(&doc, 304, 0, 576, &error) && error == PRINTER_LAYOUT_INVALID_REQUEST);
    assert(!printer_layout_create(&doc, 304, 601, 576, &error) && error == PRINTER_LAYOUT_INVALID_REQUEST);
}

static void qr_sizes(const char *folder) {
    const char *payload = "https://kubik.local/printer?size=20mm";
    printer_layout_t *layout = make((printer_document_t){.text = payload, .format = PRINTER_FORMAT_QR, .width_mm = 20}, 304, 576);
    const printer_layout_info_t *info = printer_layout_info(layout);
    assert(info->width_dots == 239 && info->height_dots == 239 && info->font_mm == 0);
    qr_geometry(layout); save_raster(layout, folder, "qr-20mm"); printer_layout_destroy(layout);
    layout = make((printer_document_t){.text = payload, .format = PRINTER_FORMAT_QR, .width_mm = 32, .height_mm = 22}, 304, 576);
    assert(printer_layout_info(layout)->height_dots == 263);
    qr_geometry(layout); save_raster(layout, folder, "qr-rectangle"); printer_layout_destroy(layout);
    rejected((printer_document_t){.text = payload, .format = PRINTER_FORMAT_QR, .width_mm = 5}, PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT);
    rejected((printer_document_t){.text = payload, .format = PRINTER_FORMAT_QR, .font_mm = 2}, PRINTER_LAYOUT_INVALID_REQUEST);
    char long_qr[258]; memset(long_qr, 'a', sizeof long_qr); long_qr[257] = 0;
    rejected((printer_document_t){.text = long_qr, .format = PRINTER_FORMAT_QR}, PRINTER_LAYOUT_INVALID_REQUEST);
    long_qr[256] = 0;
    layout = make((printer_document_t){.text = long_qr, .format = PRINTER_FORMAT_QR}, 304, 576);
    qr_geometry(layout); printer_layout_destroy(layout);
    layout = make((printer_document_t){.text = payload, .format = PRINTER_FORMAT_QR}, 72, 576);
    assert(printer_layout_info(layout)->height_mm <= 150);
    qr_geometry(layout); printer_layout_destroy(layout);
}

#include "printer_emoji_cases.h"

static void allocation_failures(void) {
    for (unsigned fail = 1; fail <= 2; ++fail) {
        allocations = 0; fail_at = fail;
        rejected((printer_document_t){.text = "Allocation lifecycle", .format = PRINTER_FORMAT_QR}, PRINTER_LAYOUT_NO_MEMORY);
    }
    fail_at = 0;
    assert(peak_bytes < 4096 && live_bytes == 0);
}

int main(int argc, char **argv) {
    text_sizes(); wraps_and_height(); input_boundaries(); ownership_and_rows();
    full_height_and_glyphs(); axis_sizes(argc > 1 ? argv[1] : NULL);
    qr_sizes(argc > 1 ? argv[1] : NULL); emoji_cases(argc > 1 ? argv[1] : NULL); allocation_failures();
    printf("printer layout: text/QR dimensions, Unicode, wrapping, fit, immutable rows and allocation cleanup passed; peak=%zu bytes\n", peak_bytes);
    return 0;
}
