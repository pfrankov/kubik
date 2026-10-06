#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/render.h"
#include "../main/icon_data.h"

#define PANEL_W (R_W * R_SCALE)
#define PANEL_H (R_H * R_SCALE)

static uint16_t full[PANEL_W * PANEL_H];
static uint16_t swapped[PANEL_W * PANEL_H];
static const uint16_t background = 0x2945;

static uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static void fill(uint16_t *pixels, uint16_t color) {
    for (int i = 0; i < PANEL_W * PANEL_H; i++) pixels[i] = color;
}

static uint32_t hash_cell(const uint16_t *pixels, int cx, int cy) {
    uint32_t hash = 2166136261u;
    for (int y = cy - 24; y < cy + 24; y++)
        for (int x = cx - 24; x < cx + 24; x++)
            hash = (hash ^ pixels[y * PANEL_W + x]) * 16777619u;
    return hash;
}

static void check_window(const scene_t *scene, const uint16_t *reference, bool swap, int x0, int y0, int width,
                         int rows) {
    uint16_t window[160 * PANEL_H];
    assert(width <= 160 && rows <= PANEL_H);
    for (int i = 0; i < width * rows; i++) window[i] = swap ? swap16(background) : background;
    render_text_overlay(scene, window, x0, y0, width, rows, swap);
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < width; x++) {
            uint16_t got = window[y * width + x], expected = reference[(y0 + y) * PANEL_W + x0 + x];
            if (got != expected) {
                fprintf(stderr, "window mismatch at %d,%d: %04x != %04x (window %d,%d %dx%d swap=%d)\n", x0 + x,
                        y0 + y, got, expected, x0, y0, width, rows, swap);
                abort();
            }
        }
    }
}

static void build_scene(scene_t *scene) {
    scene_begin(scene, background);
    assert(sc_icon(scene, -1, 10, 10, 24, 0xFFFFFF, 1) == NULL);
    assert(sc_icon(scene, ICON_COUNT, 10, 10, 24, 0xFFFFFF, 1) == NULL);
    for (int icon = 0; icon < ICON_COUNT; icon++) {
        int col = icon % 6, row = icon / 6;
        assert(sc_icon(scene, icon, 40 + col * 80, 60 + row * 120, 36, 0xEEF6F2, 1) != NULL);
    }
    assert(scene->n == ICON_COUNT);
}

static void verify_cells(void) {
    for (int icon = 0; icon < ICON_COUNT; icon++) {
        int cx = 40 + (icon % 6) * 80, cy = 60 + (icon / 6) * 120;
        int colored = 0, antialiased = 0;
        for (int y = cy - 24; y < cy + 24; y++)
            for (int x = cx - 24; x < cx + 24; x++) {
                uint16_t px = full[y * PANEL_W + x];
                if (px != background) colored++;
                if (px != background && px != rgb565(0xEEF6F2)) antialiased++;
        }
        assert(colored > 40);
        assert(antialiased > 0);
    }
}

static void verify_unique_cells(void) {
    for (int icon = 0; icon < ICON_COUNT; icon++) {
        int cx = 40 + (icon % 6) * 80, cy = 60 + (icon / 6) * 120;
        for (int previous = 0; previous < icon; previous++) {
            int pc = previous % 6, pr = previous / 6;
            assert(hash_cell(full, 40 + pc * 80, 60 + pr * 120) != hash_cell(full, cx, cy));
        }
    }
}

static void verify_swap(void) {
    for (int i = 0; i < PANEL_W * PANEL_H; i++) assert(swapped[i] == swap16(full[i]));
}

static void verify_prepared_scene(const scene_t *scene) {
    scene_t prepared = *scene;
    prepared.raster_ready = true;
    for (int i = 0; i < prepared.n; i++) {
        prim_t *p = &prepared.p[i];
        if (p->kind >= PK_SPRITE) {
            p->cx /= R_SCALE; p->cy /= R_SCALE;
            p->bx0 /= R_SCALE; p->by0 /= R_SCALE;
            p->bx1 /= R_SCALE; p->by1 /= R_SCALE;
        }
    }
    int x0, y0, x1, y1, px0, py0, px1, py1;
    uint32_t sig = render_text_signature(scene, &x0, &y0, &x1, &y1);
    assert(sig == render_text_signature(&prepared, &px0, &py0, &px1, &py1));
    assert(x0 == px0 && y0 == py0 && x1 == px1 && y1 == py1);
    fill(swapped, background);
    render_text_overlay(&prepared, swapped, 0, 0, PANEL_W, PANEL_H, false);
    assert(memcmp(swapped, full, sizeof full) == 0);
}

static void verify_windows(const scene_t *scene) {
    check_window(scene, full, false, 0, 0, 64, 64);
    check_window(scene, full, false, 21, 34, 103, 77);
    check_window(scene, full, false, 64, 146, 111, 92);
    check_window(scene, full, false, 402, 383, 78, 97);
    check_window(scene, swapped, true, 0, 0, 64, 64);
    check_window(scene, swapped, true, 21, 34, 103, 77);
    check_window(scene, swapped, true, 64, 146, 111, 92);
    check_window(scene, swapped, true, 402, 383, 78, 97);
}

// Independent pixel oracle: generated masks store the left pixel in the high
// nibble. A pair-swap can pass clipping/AA tests while visibly breaking strokes.
static void verify_source_pixels(void) {
    scene_t scene; scene_begin(&scene, background);
    sc_icon(&scene, ICON_MIC, 48, 48, ICON_MASK_SIZE, 0xFFFFFF, 1);
    fill(full, background);
    render_text_overlay(&scene, full, 0, 0, PANEL_W, PANEL_H, false);
    for (int y = 0; y < ICON_MASK_SIZE; y++) for (int x = 0; x < ICON_MASK_SIZE; x++) {
        int i = y * ICON_MASK_SIZE + x;
        unsigned coverage = (g_icon_masks[ICON_MIC][i / 2] >> (i % 2 ? 0 : 4)) & 15;
        unsigned alpha = (coverage * 255u * 32u + 1912u) / (15u * 255u);
        uint16_t expected = rgb565_blend(0xFFFF, background, alpha);
        assert(full[(y + 24) * PANEL_W + x + 24] == expected);
    }
}

int main(void) {
    scene_t scene;
    build_scene(&scene);
    fill(full, background);
    render_text_overlay(&scene, full, 0, 0, PANEL_W, PANEL_H, false);
    verify_cells();
    verify_unique_cells();
    verify_prepared_scene(&scene);
    fill(swapped, swap16(background));
    render_text_overlay(&scene, swapped, 0, 0, PANEL_W, PANEL_H, true);
    verify_swap();
    verify_windows(&scene);
    int x0, y0, x1, y1;
    uint32_t signature = render_text_signature(&scene, &x0, &y0, &x1, &y1);
    assert(signature != 2166136261u);
    assert(x0 < x1 && y0 < y1 && x0 >= 0 && y0 >= 0 && x1 <= R_W && y1 <= R_H);
    verify_source_pixels();
    printf("ok: %d unique antialiased icons, clipped windows, RGB565 swap, and dirty signature\n", ICON_COUNT);
}
