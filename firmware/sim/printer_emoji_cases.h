#pragma once

static printer_emoji_glyph_t emoji_glyph(const char *sequence, bool cross) {
    printer_emoji_glyph_t glyph = {.length=(uint8_t)strlen(sequence)};
    assert(glyph.length <= PRINTER_EMOJI_SEQUENCE_MAX_BYTES);
    memcpy(glyph.sequence, sequence, glyph.length);
    for (unsigned y = 0; y < PRINTER_EMOJI_EDGE; y++) {
        for (unsigned x = 0; x < PRINTER_EMOJI_EDGE; x++) {
            bool border = !x || !y || x == PRINTER_EMOJI_EDGE - 1 || y == PRINTER_EMOJI_EDGE - 1;
            if (border || (cross && x == y)) glyph.bitmap[y * 4 + x / 8] |= 0x80u >> (x % 8);
        }
    }
    return glyph;
}

static void emoji_sequences(void) {
    static const char *valid[] = {
        "😀", "❤", "❤️", "👩‍💻", "👍🏽", "🇷🇺", "#️⃣", "1️⃣", "👨‍👩‍👧‍👦", "🏽",
        "\U0001f3f4\U000e0067\U000e0062\U000e0073\U000e0063\U000e0074\U000e007f",
    };
    static const char *invalid[] = {
        "", "A", "😀😀", "🏽🏻", "\U0001f1f7", "👩\u200d", "👩\u200dA",
        "❤\ufe0f\ufe0f", "\xc0\xaf", "\xed\xa0\x80", "\U0001f3f4\U000e0067", "1",
    };
    for (unsigned i = 0; i < sizeof valid / sizeof *valid; i++)
        assert(printer_emoji_sequence_valid(valid[i], strlen(valid[i])));
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++)
        assert(!printer_emoji_sequence_valid(invalid[i], strlen(invalid[i])));
    assert(!printer_emoji_sequence_valid(NULL, 4));
    assert(!printer_emoji_sequence_valid("😀", 65));
}

static void emoji_map_validation(void) {
    printer_emoji_glyph_t glyphs[2] = {emoji_glyph("😀", true), emoji_glyph("❤️", false)};
    assert(printer_emoji_valid(glyphs, 2));
    assert(printer_emoji_valid(NULL, 0));
    assert(!printer_emoji_valid(NULL, 1));
    assert(!printer_emoji_valid(glyphs, PRINTER_EMOJI_MAX + 1));
    glyphs[1] = glyphs[0]; assert(!printer_emoji_valid(glyphs, 2));
    glyphs[0].length = 0; assert(!printer_emoji_valid(glyphs, 1));
    glyphs[0].length = 65; assert(!printer_emoji_valid(glyphs, 1));
    glyphs[0] = emoji_glyph("😀", true);
    glyphs[0].sequence[1] = 0; assert(!printer_emoji_valid(glyphs, 1));
    glyphs[0] = emoji_glyph("😀", true);
    memset(glyphs[0].bitmap, 0, PRINTER_EMOJI_BYTES); assert(!printer_emoji_valid(glyphs, 1));
}

static void emoji_longest_and_boundary(void) {
    printer_emoji_glyph_t glyphs[] = {emoji_glyph("👩", true), emoji_glyph("👩‍💻", false),
        emoji_glyph("❤", true), emoji_glyph("❤️", false), emoji_glyph("👍", true)};
    const char *text = "👩‍💻X";
    assert(printer_emoji_match(glyphs, 5, text, strlen(text)) == &glyphs[1]);
    text = "❤️❤";
    assert(printer_emoji_match(glyphs, 5, text, strlen(text)) == &glyphs[3]);
    text = "👩‍💻";
    assert(!printer_emoji_match(glyphs, 1, text, strlen(text)));
    text = "👍🏽";
    assert(!printer_emoji_match(glyphs, 5, text, strlen(text)));
    text = "❤\u0301";
    assert(!printer_emoji_match(glyphs, 5, text, strlen(text)));
    assert(!printer_emoji_pixel(NULL, 0, 0));
    assert(!printer_emoji_pixel(glyphs, 32, 0));
    assert(!printer_emoji_pixel(glyphs, 0, 32));
}

static void emoji_tag_boundary(void) {
    const char *tag = "\U0001f3f4\U000e0067\U000e0062\U000e0073\U000e0063\U000e0074\U000e007f";
    printer_emoji_glyph_t glyphs[] = {emoji_glyph("🏴", true), emoji_glyph(tag, false)};
    assert(printer_emoji_valid(glyphs, 2));
    assert(printer_emoji_match(glyphs, 2, tag, strlen(tag)) == &glyphs[1]);
    assert(!printer_emoji_match(glyphs, 1, tag, strlen(tag)));
}

static void emoji_mixed_text(const char *folder) {
    printer_emoji_glyph_t glyphs[] = {emoji_glyph("😀", true), emoji_glyph("👩", true),
        emoji_glyph("👩‍💻", false), emoji_glyph("👍🏽", true), emoji_glyph("🇷🇺", false),
        emoji_glyph("❤", true), emoji_glyph("❤️", false), emoji_glyph("#️⃣", true)};
    printer_document_t doc = {.text="Кубик A 😀 👩‍💻 👍🏽 🇷🇺 ❤️ #️⃣", .width_mm=40,
        .font_mm=3, .emojis=glyphs, .emoji_count=8};
    unsigned before = allocations;
    printer_layout_t *layout = make_axes(doc, 304, 152);
    assert(allocations == before + 1); // No second heap copy of the job-owned glyphs.
    assert(layout->emojis == glyphs && layout->has_emoji);
    complete_text(layout); inspect(layout); save_raster(layout, folder, "text-emoji");
    printer_layout_destroy(layout);
    doc.text = "👩‍💻"; doc.emoji_count = 2;
    rejected(doc, PRINTER_LAYOUT_UNSUPPORTED_CHARACTER); // A supplied base is never a composite substitute.
    doc.text = "👍🏽"; glyphs[0] = emoji_glyph("👍", true); doc.emoji_count = 1;
    rejected(doc, PRINTER_LAYOUT_UNSUPPORTED_CHARACTER);
    doc.text = "❤️"; glyphs[0] = emoji_glyph("❤", true);
    rejected(doc, PRINTER_LAYOUT_UNSUPPORTED_CHARACTER);
    doc.text = "X"; doc.format = PRINTER_FORMAT_QR; doc.font_mm = 0;
    rejected(doc, PRINTER_LAYOUT_INVALID_REQUEST);
}

static void emoji_box_and_wrap(void) {
    printer_emoji_glyph_t glyph = emoji_glyph("😀", true);
    printer_document_t doc = {.text="😀", .font_mm=3, .emojis=&glyph, .emoji_count=1};
    printer_layout_t *layout = make_axes(doc, 304, 152);
    ink_t ink = inspect(layout);
    unsigned width = ink.right - ink.left + 1, height = ink.bottom - ink.top + 1;
    assert(width == layout->emoji_w && height == layout->emoji_h && width == 2 * height);
    assert(ink.left >= layout->pad_x && ink.top >= layout->pad_y);
    assert(layout->info.height_dots - ink.bottom - 1 >= layout->pad_y);
    printer_layout_destroy(layout);
    doc.text = "😀😀😀"; doc.width_mm = 10;
    layout = make_axes(doc, 304, 152);
    assert(layout->info.lines == 3);
    for (unsigned i = 0; i < layout->info.lines; i++)
        assert(layout->payload.lines[i].end - layout->payload.lines[i].start == glyph.length);
    complete_text(layout); inspect(layout); printer_layout_destroy(layout);
    glyph = emoji_glyph("👩‍💻", true); doc.text = "👩‍💻👩‍💻"; doc.width_mm = 7;
    layout = make_axes(doc, 304, 152);
    assert(layout->info.lines == 2);
    complete_text(layout); inspect(layout); printer_layout_destroy(layout);
    doc.width_mm = 1; rejected(doc, PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT);
}

static void emoji_flag_wrap_and_fit(void) {
    printer_emoji_glyph_t glyph = emoji_glyph("🇷🇺", true);
    printer_document_t doc = {.text="🇷🇺🇷🇺", .font_mm=3, .width_mm=7, .emojis=&glyph, .emoji_count=1};
    printer_layout_t *layout = make_axes(doc, 304, 152);
    assert(layout->info.lines == 2);
    assert(layout->payload.lines[0].end == glyph.length);
    complete_text(layout); inspect(layout); printer_layout_destroy(layout);
    glyph = emoji_glyph("😀", true); doc.text = "😀"; doc.width_mm = 20;
    doc.font_mm = 6; doc.height_mm = 6;
    layout = make_axes(doc, 304, 152);
    assert(layout->info.font_mm < 6 && layout->info.font_mm >= 1.5);
    assert(layout->info.height_dots == 36);
    inspect(layout); printer_layout_destroy(layout);
    doc.height_mm = 3; rejected(doc, PRINTER_LAYOUT_CONTENT_DOES_NOT_FIT);
}

static void unused_emoji_does_not_change_text(void) {
    printer_emoji_glyph_t glyph = emoji_glyph("😀", true);
    printer_document_t doc = {.text="A", .font_mm=3};
    printer_layout_t *layout = make_axes(doc, 304, 152);
    ink_t expected = inspect(layout);
    unsigned expected_height = layout->info.height_dots;
    printer_layout_destroy(layout);
    doc.emojis = &glyph; doc.emoji_count = 1;
    layout = make_axes(doc, 304, 152);
    assert(!layout->has_emoji && layout->info.height_dots == expected_height);
    assert(inspect(layout).hash == expected.hash);
    printer_layout_destroy(layout);
    doc.emojis = NULL; rejected(doc, PRINTER_LAYOUT_INVALID_REQUEST);
}

static void emoji_cases(const char *folder) {
    emoji_sequences(); emoji_map_validation(); emoji_longest_and_boundary(); emoji_tag_boundary();
    emoji_mixed_text(folder); emoji_box_and_wrap(); emoji_flag_wrap_and_fit();
    unused_emoji_does_not_change_text();
}
