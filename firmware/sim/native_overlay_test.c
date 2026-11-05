// Native alpha overlays: independent source sampling and window ownership.
#include "../main/render.h"
#include "../main/render_projection.h"
#include "../main/icon_data.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIDTH UI_RENDER_W
#define HEIGHT UI_OUTPUT_H
static uint16_t whole[HEIGHT][WIDTH], tiled[HEIGHT][WIDTH];
static uint8_t modules[65 * 65];

static void bounds(prim_t *p, int x, int y, int width, int height) {
    p->bx0 = (int16_t)x; p->by0 = (int16_t)y;
    p->bx1 = (int16_t)(x + width - 1); p->by1 = (int16_t)(y + height - 1);
}
static unsigned sample(const font_t *font, const font_glyph_t *glyph, int x, int y) {
    if (x < 0 || y < 0 || x >= glyph->w || y >= glyph->h) return 0;
    unsigned byte = font->px[glyph->off + y * ((glyph->w + 1) / 2) + x / 2];
    return (byte >> ((x & 1) * 4)) & 15;
}
static unsigned mask_value(const uint8_t *mask, int x, int y) {
    x = x < 0 ? 0 : x >= ICON_MASK_SIZE ? ICON_MASK_SIZE - 1 : x;
    y = y < 0 ? 0 : y >= ICON_MASK_SIZE ? ICON_MASK_SIZE - 1 : y;
    int index = y * ICON_MASK_SIZE + x;
    return (mask[index / 2] >> ((index & 1) ? 0 : 4)) & 15;
}
static double design(int physical) { return floor((((physical + .5) * 30 / 23) - .5) * 256) / 256; }
static uint16_t expected(double coverage, uint8_t opacity, uint16_t color, uint16_t bg) {
    unsigned q8 = (unsigned)floor(coverage * 256 + .5);
    unsigned alpha = (q8 * opacity * 32 + 489600) / 979200;
    return alpha == 32 ? color : rgb565_blend(color, bg, alpha);
}
static void fonts(void) {
    uint16_t pixels[80 * 80]; unsigned checked = 0;
    for (int f = 0; f < FONT_COUNT; f++) {
        const font_t *font = &g_fonts[f];
        for (int g = 0; g < font->n; g++) {
            const font_glyph_t *glyph = &font->g[g];
            scene_t scene = {.n = 1}; scene.text[0] = (uint8_t)g;
            scene.p[0] = (prim_t){.kind = PK_TEXT, .alpha = 193, .color = 0xE596, .r = f, .b = 1,
                .sub_x = 100 - glyph->x, .sub_y = 100 + glyph->top};
            bounds(&scene.p[0], 99, 99, glyph->w + 2, glyph->h + 2);
            for (unsigned i = 0; i < 80 * 80; i++) pixels[i] = 0x3277;
            render_text_overlay(&scene, pixels, 70, 70, 80, 80, 80, false);
            for (int y = 70; y < 150; y++) for (int x = 70; x < 150; x++) {
                double sx = design(x) - 100, sy = design(y) - 100;
                int ix = (int)floor(sx), iy = (int)floor(sy); double fx = sx - ix, fy = sy - iy;
                double c = sample(font, glyph, ix, iy) * (1 - fx) * (1 - fy) + sample(font, glyph, ix + 1, iy) * fx * (1 - fy)
                    + sample(font, glyph, ix, iy + 1) * (1 - fx) * fy + sample(font, glyph, ix + 1, iy + 1) * fx * fy;
                assert(pixels[(y - 70) * 80 + x - 70] == expected(c, 193, 0xE596, 0x3277));
            }
            checked++;
        }
    }
    printf("native font: %u glyphs match independent Q8 source coverage, transparent outside and opacity\n", checked);
}
static void icons(void) {
    uint16_t pixels[64 * 64];
    for (int icon = 0; icon < ICON_COUNT; icon++) {
        scene_t scene = {.n = 1};
        scene.p[0] = (prim_t){.kind = PK_ICON, .alpha = 219, .color = 0x2FFD, .a = 40 * 16,
            .b = icon, .sub_x = 128 * 16, .sub_y = 128 * 16};
        bounds(&scene.p[0], 108, 108, 40, 40);
        memset(pixels, 0, sizeof pixels); render_text_overlay(&scene, pixels, 65, 65, 64, 64, 64, false);
        for (int y = 65; y < 129; y++) for (int x = 65; x < 129; x++) {
            double cx = design(x) + .5, cy = design(y) + .5;
            double sx = floor(((cx - 108) * 48 / 40 - .5) * 256) / 256;
            double sy = floor(((cy - 108) * 48 / 40 - .5) * 256) / 256;
            int ix = (int)floor(sx), iy = (int)floor(sy); double fx = sx - ix, fy = sy - iy;
            double c = mask_value(g_icon_masks[icon], ix, iy) * (1 - fx) * (1 - fy)
                + mask_value(g_icon_masks[icon], ix + 1, iy) * fx * (1 - fy)
                + mask_value(g_icon_masks[icon], ix, iy + 1) * (1 - fx) * fy
                + mask_value(g_icon_masks[icon], ix + 1, iy + 1) * fx * fy;
            // The source mask has a transparent margin; no extrapolated ink exists outside its footprint.
            assert(pixels[(y - 65) * 64 + x - 65] == expected(c, 219, 0x2FFD, 0));
        }
    }
    puts("native icons: all masks match independent source sampling");
}
static void draw(const scene_t *scene, uint16_t *out, int x0, int y0, int width, int rows, bool swap) {
    render_edge_overlay(scene, out, x0, y0, width, rows, width, swap);
    render_text_overlay(scene, out, x0, y0, width, rows, width, swap);
}
static void fixture(scene_t *scene) {
    *scene = (scene_t){.n = 3, .qr = modules, .edge_w = 7, .edge_r = 40, .edge_color = 0x36DA};
    scene->p[0] = (prim_t){.kind = PK_QR, .alpha = 255, .color = 0xFFFF, .a = 41, .b = 287, .cx = 52, .cy = 104, .sub_x = 40, .sub_y = 80};
    scene->p[1] = (prim_t){.kind = PK_TEXT, .alpha = 193, .color = 0xFD40, .r = FONT_CARD, .b = 1, .sub_x = 474, .sub_y = 582};
    scene->text[0] = (uint8_t)font_glyph(&g_fonts[FONT_CARD], 'W');
    scene->p[2] = (prim_t){.kind = PK_ICON, .alpha = 230, .color = 0x2FFD, .a = 60 * 16,
        .b = ICON_MIC, .sub_x = 476 * 16, .sub_y = 8 * 16};
    bounds(&scene->p[0], 52, 104, 376, 376);
    bounds(&scene->p[1], 470, 530, 60, 60);
    bounds(&scene->p[2], 446, -22, 60, 60);
    for (int y = 0; y < 41; y++) for (int x = 0; x < 41; x++) modules[y * 41 + x] = (uint8_t)((x + y * 3) & 1);
}
static void check_padding(void) {
    for (int y = 0; y < HEIGHT; y++) for (int x = UI_OUTPUT_W; x < WIDTH; x++) assert(whole[y][x] == 0x1234);
}
static void windows(void) {
    scene_t scene; fixture(&scene);
    for (int swap = 0; swap < 2; swap++) {
        for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < WIDTH; x++) whole[y][x] = tiled[y][x] = 0x1234;
        draw(&scene, &whole[0][0], 0, 0, WIDTH, HEIGHT, swap);
        for (int y = 0; y < HEIGHT; y += 7) for (int x = 0; x < WIDTH; x += 13) {
            int width = x + 13 < WIDTH ? 13 : WIDTH - x, rows = y + 7 < HEIGHT ? 7 : HEIGHT - y;
            uint16_t tile[13 * 7]; for (int i = 0; i < width * rows; i++) tile[i] = 0x1234;
            draw(&scene, tile, x, y, width, rows, swap);
            for (int j = 0; j < rows; j++) memcpy(&tiled[y + j][x], tile + j * width, width * 2);
        }
        assert(!memcmp(whole, tiled, sizeof whole));
        check_padding();
    }
    render_overlay_item_t item; assert(render_overlay_item(&scene, &scene.p[0], &item));
    modules[0] ^= 1; render_overlay_item_t changed; assert(render_overlay_item(&scene, &scene.p[0], &changed));
    assert(changed.signature != item.signature && item.x0 < item.x1 && item.y0 < item.y1);
    puts("native overlays: arbitrary tiles equal whole frame, wire order and right padding safe, QR content dirties");
}
static void strided_overlay_window(const scene_t *scene, int x0, int y0, int width, int rows, bool swap) {
    static uint16_t guarded[8 + WIDTH * HEIGHT + 8];
    for (unsigned i = 0; i < sizeof guarded / sizeof guarded[0]; i++) guarded[i] = 0xA55A;
    for (int y = 0; y < rows; y++) for (int x = x0; x < x0 + width; x++) guarded[8 + y * WIDTH + x] = 0x1234;
    render_edge_overlay(scene, guarded + 8 + x0, x0, y0, width, rows, WIDTH, swap);
    render_text_overlay(scene, guarded + 8 + x0, x0, y0, width, rows, WIDTH, swap);
    for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < WIDTH; x++) {
        uint16_t want = y < rows && x >= x0 && x < x0 + width ? whole[y0 + y][x] : 0xA55A;
        assert(guarded[8 + y * WIDTH + x] == want);
    }
    for (int i = 0; i < 8; i++) {
        assert(guarded[i] == 0xA55A);
        assert(guarded[8 + WIDTH * HEIGHT + i] == 0xA55A);
    }
}
static void strided_overlays(void) {
    scene_t scene; fixture(&scene);
    for (int projected = 0; projected < 2; projected++) {
        if (projected) render_project_scene(&scene);
        for (int swap = 0; swap < 2; swap++) {
            for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < WIDTH; x++) whole[y][x] = 0x1234;
            draw(&scene, &whole[0][0], 0, 0, WIDTH, HEIGHT, swap);
            strided_overlay_window(&scene, 0, 0, UI_OUTPUT_W, HEIGHT, swap);
            strided_overlay_window(&scene, 14, 60, 330, 310, swap);
            strided_overlay_window(&scene, 356, 432, 12, 16, swap);
        }
    }
    puts("native overlay stride: full/partial/tail windows match packed reference; all row and outer canaries intact");
}
static void qr_pixels(void) {
    for (int n = 21; n <= 65; n += 4) {
        int side = 368;
        int ox = (368 - side) / 2, oy = (448 - side) / 2;
        scene_t scene = {.n = 1, .qr = modules};
        scene.p[0] = (prim_t){.kind = PK_QR, .alpha = 255, .color = 0xFFFF, .a = n, .b = side, .sub_x = ox, .sub_y = oy};
        bounds(&scene.p[0], 0, 0, UI_W, UI_H);
        for (int i = 0; i < n * n; i++) modules[i] = (uint8_t)((i * 17 + i / n) & 1);
        memset(whole, 0, sizeof whole); render_edge_overlay(&scene, &whole[0][0], 0, 0, WIDTH, HEIGHT, WIDTH, false);
        for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < UI_OUTPUT_W; x++) {
            bool inside = x >= ox && x < ox + side && y >= oy && y < oy + side;
            uint16_t want = inside && modules[((y - oy) * n / side) * n + (x - ox) * n / side] ? 0xFFFF : 0;
            assert(whole[y][x] == want);
        }
    }
    puts("native QR: source sizes 21..65 match crisp integer pixel source cells without smoothing");
}
static void qr_fade(void) {
    uint16_t pixels[32 * 16]; scene_t scene = {.n = 1, .qr = modules};
    memset(modules, 1, sizeof modules);
    scene.p[0] = (prim_t){.kind = PK_QR, .color = 0xE593, .a = 41, .b = 287, .cx = 52, .cy = 104, .sub_x = 40, .sub_y = 80};
    bounds(&scene.p[0], 76, 126, 328, 328);
    for (int alpha = 0; alpha <= 255; alpha++) for (int swap = 0; swap < 2; swap++) {
        scene.p[0].alpha = (uint8_t)alpha;
        for (int i = 0; i < 32 * 16; i++) pixels[i] = swap ? rgb565_bswap(0x3277) : 0x3277;
        render_edge_overlay(&scene, pixels, 100, 120, 32, 16, 32, swap);
        uint16_t want = expected(15, (uint8_t)alpha, 0xE593, 0x3277);
        if (swap) want = rgb565_bswap(want);
        for (int i = 0; i < 32 * 16; i++) assert(pixels[i] == want);
    }
    puts("native QR: every fade opacity preserves exact RGB565 compositing in both byte orders");
}
static void check_edge_band(int before, int after, int y0) {
    edge_change_t c = render_edge_change(before, after, 40, y0, y0 + 16);
    int reach = render_edge_reach(before > after ? before : after, 40, y0, y0 + 16);
    for (int y = y0; y < y0 + 16; y++) for (int x = 0; x < UI_OUTPUT_W; x++) {
        if (whole[y][x] == tiled[y][x]) continue;
        int mirror = x < UI_OUTPUT_W / 2 ? x : UI_OUTPUT_W - 1 - x;
        assert(mirror < reach);
        bool side = mirror >= c.x0 && mirror < c.x1;
        bool horizontal = y >= c.sy0 && y < c.sy1 && x >= c.r && x < UI_OUTPUT_W - c.r;
        assert(side || horizontal);
    }
}
static void check_symmetry(void) {
    for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < UI_OUTPUT_W; x++)
        assert(tiled[y][x] == tiled[HEIGHT - 1 - y][UI_OUTPUT_W - 1 - x]);
}
static void check_ink_bounds(const render_overlay_item_t *item) {
    for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < UI_OUTPUT_W; x++) {
        if (!whole[y][x]) continue;
        assert(x / 2 >= item->x0 && x / 2 < item->x1);
        assert(y / 2 >= item->y0 && y / 2 < item->y1);
    }
}
static void source_bounds(void) {
    static const int positions[][2] = {{-8, 15}, {240, 292}, {474, 582}};
    memset(modules, 1, sizeof modules);
    for (int kind = 0; kind < 3; kind++) for (unsigned k = 0; k < sizeof positions / sizeof positions[0]; k++) {
        scene_t scene; scene_begin(&scene, 0); int x = positions[k][0], y = positions[k][1];
        prim_t *p;
        if (!kind) p = sc_text(&scene, FONT_CARD, "Agyj", x, y, -1, 0, 0xFFFFFF, 1);
        else if (kind == 1) p = sc_icon(&scene, ICON_CHECK, x, y, 40, 0xFFFFFF, 1);
        else p = sc_qr(&scene, modules, 41, 164, x, y, 0xFFFFFF, 1);
        render_overlay_item_t before, after;
        assert(render_overlay_item(&scene, p, &before));
        render_project_scene(&scene); assert(render_overlay_item(&scene, p, &after));
        assert(!memcmp(&before, &after, sizeof before));
        memset(whole, 0, sizeof whole); draw(&scene, &whole[0][0], 0, 0, WIDTH, HEIGHT, false);
        check_ink_bounds(&after);
    }
    puts("native bounds: actual builders before/after projection agree, all clipped font/icon/QR ink covered");
}
static void edge_damage(void) {
    static const int widths[] = {0, 3, 7, 15, 64, 128, 255};
    for (unsigned a = 0; a < sizeof widths / sizeof widths[0]; a++) for (unsigned b = 0; b < sizeof widths / sizeof widths[0]; b++) {
        int before = widths[a], after = widths[b];
        if (before && after && render_edge_radius(before, 40) != render_edge_radius(after, 40)) continue;
        scene_t scene = {.edge_w = before, .edge_r = 40, .edge_color = 0xFFFF};
        memset(whole, 0, sizeof whole); render_edge_overlay(&scene, &whole[0][0], 0, 0, WIDTH, HEIGHT, WIDTH, false);
        scene.edge_w = after; memset(tiled, 0, sizeof tiled); render_edge_overlay(&scene, &tiled[0][0], 0, 0, WIDTH, HEIGHT, WIDTH, false);
        for (int y0 = 0; y0 < HEIGHT; y0 += 16) check_edge_band(before, after, y0);
        check_symmetry();
    }
    puts("native edge: physical symmetry and every changed pixel covered by bounded damage");
}
int main(void) { fonts(); icons(); windows(); strided_overlays(); qr_pixels(); qr_fade(); source_bounds(); edge_damage(); return 0; }
