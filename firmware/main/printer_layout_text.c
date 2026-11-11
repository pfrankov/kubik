#include "printer_layout_internal.h"

#include <math.h>
#include <string.h>

typedef struct { int pen, left, right, previous; } text_span_t;
typedef struct { unsigned end, next; bool complete; } line_scan_t;
typedef struct { uint32_t cp; const printer_emoji_glyph_t *emoji; } text_token_t;

static int maximum(int a, int b) { return a > b ? a : b; }
static int minimum(int a, int b) { return a < b ? a : b; }
static uint32_t normalized(uint32_t cp) { return cp == 0xa0 ? ' ' : cp; }
static bool horizontal(uint32_t cp) { return cp == ' ' || cp == '\t'; }
static int scaled_q8(const printer_layout_t *layout, int q4) { return q4 * layout->cap_x * 16 / layout->font->cap; }

static int next_tab(const printer_layout_t *layout, int pen) {
    int space = font_glyph(layout->font, ' ');
    int tab = 4 * scaled_q8(layout, layout->font->g[space].adv);
    return (pen / tab + 1) * tab;
}

static void add_character(const printer_layout_t *layout, text_span_t *span, uint32_t cp) {
    if (cp == '\t') { span->pen = next_tab(layout, span->pen); span->previous = -1; }
    else {
        int index = font_glyph(layout->font, normalized(cp));
        const font_glyph_t *glyph = &layout->font->g[index];
        if (span->previous >= 0) span->pen += scaled_q8(layout, font_kern(layout->font, span->previous, index));
        int x = (span->pen + 128) / 256;
        span->left = minimum(span->left, x + printer_scaled_floor(glyph->x, layout->cap_x, layout->font->cap));
        span->right = maximum(span->right, x + printer_scaled_ceil(glyph->x + glyph->w, layout->cap_x, layout->font->cap));
        span->pen += scaled_q8(layout, glyph->adv);
        span->previous = index;
    }
    span->right = maximum(span->right, (span->pen + 255) / 256);
}

static text_token_t next_token(const printer_layout_t *layout, const char **cursor) {
    size_t left = layout->text_bytes - (size_t)(*cursor - layout->text);
    const printer_emoji_glyph_t *emoji = printer_emoji_match(layout->emojis, layout->emoji_count, *cursor, left);
    if (emoji) { *cursor += emoji->length; return (text_token_t){.emoji=emoji}; }
    return (text_token_t){.cp=font_utf8_next(cursor)};
}

static void add_token(const printer_layout_t *layout, text_span_t *span, text_token_t token) {
    if (!token.emoji) { add_character(layout, span, token.cp); return; }
    int x = (span->pen + 128) / 256;
    span->left = minimum(span->left, x);
    span->right = maximum(span->right, x + layout->emoji_w);
    span->pen += (layout->emoji_w + (layout->cap_x + 9) / 10) * 256;
    span->right = maximum(span->right, (span->pen + 255) / 256);
    span->previous = -1;
}

static bool supported(printer_layout_t *layout) {
    const char *cursor = layout->text;
    while (*cursor) {
        text_token_t token = next_token(layout, &cursor);
        if (token.emoji) { layout->has_emoji = true; continue; }
        if (token.cp == '\n' || token.cp == '\t') continue;
        uint32_t cp = normalized(token.cp);
        int index = font_glyph(layout->font, cp);
        if (layout->font->g[index].cp != cp) return false;
    }
    return true;
}

static unsigned skip_horizontal(const char *text, unsigned at) {
    while (text[at] == ' ' || text[at] == '\t') ++at;
    return at;
}

static bool wrapped_line(const printer_layout_t *layout, unsigned start, unsigned at,
    unsigned space_end, unsigned space_next, bool has_space, line_scan_t *result) {
    if (has_space && space_end > start) {
        *result = (line_scan_t){space_end, skip_horizontal(layout->text, space_next), false};
        result->complete = !layout->text[result->next];
        return true;
    }
    if (at == start) return false;
    *result = (line_scan_t){at, at, false};
    return true;
}

static bool scan_line(const printer_layout_t *layout, unsigned start, line_scan_t *result) {
    text_span_t span = {.previous = -1};
    unsigned at = start, space_end = start, space_next = start;
    bool has_space = false;
    int room = layout->info.width_dots - 2 * layout->pad_x;
    while (layout->text[at] && layout->text[at] != '\n') {
        const char *next = layout->text + at;
        text_token_t token = next_token(layout, &next);
        if (!token.emoji && horizontal(token.cp)) { space_end = at; space_next = next - layout->text; has_space = true; }
        add_token(layout, &span, token);
        if (span.right - span.left > room)
            return wrapped_line(layout, start, at, space_end, space_next, has_space, result);
        at = next - layout->text;
    }
    *result = (line_scan_t){at, at + (layout->text[at] == '\n'), !layout->text[at]};
    return true;
}

static bool add_line(printer_layout_t *layout, unsigned start, unsigned end) {
    if (layout->info.lines == PRINTER_LAYOUT_MAX_LINES) return false;
    while (end > start && (layout->text[end - 1] == ' ' || layout->text[end - 1] == '\t')) --end;
    text_span_t span = {.previous = -1};
    const char *cursor = layout->text + start;
    while (cursor < layout->text + end) add_token(layout, &span, next_token(layout, &cursor));
    layout->payload.lines[layout->info.lines++] = (printer_line_t){start, end, layout->pad_x - span.left};
    return true;
}

static bool plan_lines(printer_layout_t *layout) {
    unsigned start = 0;
    layout->info.lines = 0;
    for (;;) {
        line_scan_t line;
        if (!scan_line(layout, start, &line) || !add_line(layout, start, line.end)) return false;
        if (line.complete) return true;
        start = line.next;
    }
}

static void font_metrics(printer_layout_t *layout) {
    int ascent = layout->font->ascent, descent = layout->font->descent;
    for (unsigned i = 0; i < layout->font->n; ++i) {
        const font_glyph_t *g = &layout->font->g[i];
        ascent = maximum(ascent, g->top);
        descent = maximum(descent, (int)g->h - g->top);
    }
    layout->emoji_h = (layout->cap_y * 135 + 50) / 100;
    layout->emoji_w = (uint16_t)lround(layout->emoji_h * (double)layout->info.dpi / layout->info.feed_dpi);
    layout->ascent = printer_scaled_ceil(ascent, layout->cap_y, layout->font->cap);
    if (layout->has_emoji) layout->ascent = maximum(layout->ascent, layout->emoji_h);
    layout->descent = printer_scaled_ceil(descent, layout->cap_y, layout->font->cap);
    layout->line_step = layout->ascent + layout->descent + (layout->cap_y + 4) / 5;
}

static bool fit_cap(printer_layout_t *layout, unsigned cap, unsigned height_limit, unsigned *height) {
    layout->cap_y = cap;
    layout->cap_x = (uint16_t)lround(cap * (double)layout->info.dpi / layout->info.feed_dpi);
    font_metrics(layout);
    if (!plan_lines(layout)) return false;
    *height = 2 * layout->pad_y + layout->ascent + layout->descent +
        (layout->info.lines - 1) * layout->line_step;
    return *height <= height_limit;
}

bool printer_layout_text(printer_layout_t *layout, const printer_document_t *doc, printer_layout_error_t *error) {
    layout->font = &g_fonts[FONT_TITLE];
    if (!supported(layout)) { *error = PRINTER_LAYOUT_UNSUPPORTED_CHARACTER; return false; }
    unsigned dpi = layout->info.feed_dpi;
    layout->pad_x = (uint16_t)lround(layout->info.dpi / 25.4);
    layout->pad_y = (uint16_t)lround(dpi / 25.4);
    unsigned height_limit = layout->info.height_dots;
    if (!height_limit) height_limit = (unsigned)floor(PRINTER_LAYOUT_MAX_HEIGHT_MM * dpi / 25.4);
    int cap_min = (int)ceil(1.5 * dpi / 25.4), cap_max = (int)floor(8 * dpi / 25.4);
    int cap = (int)lround((doc->font_mm ? doc->font_mm : 2.5) * dpi / 25.4);
    cap = maximum(cap_min, minimum(cap, cap_max));
    int last_cap = doc->font_mm && !doc->height_mm ? cap : cap_min;
    unsigned height;
    for (; cap >= last_cap; --cap) {
        if (!fit_cap(layout, cap, height_limit, &height)) continue;
        if (!layout->info.height_dots) layout->info.height_dots = height;
        layout->info.font_mm = layout->cap_y * 25.4 / dpi;
        return true;
    }
    *error = PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT;
    return false;
}

static void draw_glyph(const printer_layout_t *layout, const font_glyph_t *glyph,
    int pen, int relative_y, uint8_t *row) {
    int cap_x = layout->cap_x, cap_y = layout->cap_y, base = layout->font->cap;
    int sy = printer_scaled_floor(2 * relative_y + 1, base, 2 * cap_y) + glyph->top;
    if (sy < 0 || sy >= glyph->h) return;
    int left = pen + printer_scaled_floor(glyph->x, cap_x, base);
    int right = pen + printer_scaled_ceil(glyph->x + glyph->w, cap_x, base);
    const uint8_t *pixels = layout->font->px + glyph->off + sy * ((glyph->w + 1) / 2);
    for (int x = left; x < right; ++x) {
        int sx = printer_scaled_floor(2 * (x - pen) + 1, base, 2 * cap_x) - glyph->x;
        if (sx < 0 || sx >= glyph->w) continue;
        unsigned coverage = pixels[sx / 2] >> ((sx % 2) * 4) & 15;
        if (coverage >= 8) printer_black(row, x);
    }
}

static void draw_emoji(const printer_layout_t *layout, const printer_emoji_glyph_t *glyph,
    int pen, int relative_y, uint8_t *row) {
    int y = relative_y + layout->emoji_h;
    if (y < 0 || y >= layout->emoji_h) return;
    unsigned source_y = (2 * y + 1) * PRINTER_EMOJI_EDGE / (2 * layout->emoji_h);
    for (unsigned x = 0; x < layout->emoji_w; x++) {
        unsigned source_x = (2 * x + 1) * PRINTER_EMOJI_EDGE / (2 * layout->emoji_w);
        if (printer_emoji_pixel(glyph, source_x, source_y)) printer_black(row, pen + x);
    }
}

static void draw_line(const printer_layout_t *layout, unsigned index, int relative_y, uint8_t *row) {
    const printer_line_t *line = &layout->payload.lines[index];
    const char *cursor = layout->text + line->start;
    text_span_t span = {.previous=-1};
    while (cursor < layout->text + line->end) {
        text_token_t token = next_token(layout, &cursor);
        if (token.emoji) {
            draw_emoji(layout, token.emoji, line->shift + (span.pen + 128) / 256, relative_y, row);
        } else if (token.cp != '\t') {
            int glyph = font_glyph(layout->font, normalized(token.cp));
            int kern = span.previous < 0 ? 0 : scaled_q8(layout, font_kern(layout->font, span.previous, glyph));
            draw_glyph(layout, &layout->font->g[glyph], line->shift + (span.pen + kern + 128) / 256, relative_y, row);
        }
        add_token(layout, &span, token);
    }
}

void printer_text_row(const printer_layout_t *layout, unsigned y, uint8_t *row) {
    for (unsigned i = 0; i < layout->info.lines; ++i) {
        int baseline = layout->pad_y + layout->ascent + i * layout->line_step;
        int relative = (int)y - baseline;
        if (relative >= -(int)layout->ascent && relative < layout->descent) draw_line(layout, i, relative, row);
    }
}
