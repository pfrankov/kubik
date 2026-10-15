// One-off palette sampling for native behavioral fixtures.
static uint32_t tess_color(const face_t *face, float depth) {
    palette_t palette = prepare_palette(face);
    return palette_color(face, depth, &palette);
}
