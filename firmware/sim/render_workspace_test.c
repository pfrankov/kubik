// Exact-sized rolling workspace against independent full-eight-row raster calls.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../main/render_internal.h"
#include "../main/canvas.h"

_Static_assert(sizeof(render_workspace_t) == R_W * (R_BAND + 1) * 2, "no hidden ring storage");
_Static_assert(_Alignof(render_workspace_t) >= 4, "canvas pair alignment");
#if CONFIG_IDF_TARGET_ESP32S3
_Static_assert(sizeof(render_workspace_t) == 3456, "S3 workspace bytes");
#else
_Static_assert(sizeof(render_workspace_t) == 4320, "C6 workspace bytes");
#endif

typedef struct {
    uint32_t before[4];
    render_workspace_t workspace;
    uint32_t after[4];
} guarded_t;
static guarded_t guarded;
static scene_t scene, oracle;
static canvas_t actual_diff, expected_diff;
static uint16_t expected[R_H][R_W], captured[R_H][R_W];
static unsigned visits[R_H], image_visits[R_H];
static int pushed, ready_count;
static bool count_image;

static void check_guards(void) {
    for (int i = 0; i < 4; i++) {
        assert(guarded.before[i] == 0x93BAD01Fu);
        assert(guarded.after[i] == 0x125AC37Eu);
    }
}
static void image(const prim_t *p, uint16_t *out, int x0, int width, int ya, int yb, int origin) {
    (void)p;
    for (int y = ya; y <= yb; y++) {
        if (count_image) image_visits[y]++;
        for (int x = x0; x < x0 + width; x++) {
            uint16_t value = (uint16_t)((x * 197u) ^ (y * 571u) ^ (y % 8 ? 0xA583 : 0x39E1));
            out[(y - origin) * width + x - x0] = g_render_swap ? rgb565_bswap(value) : value;
        }
    }
}
static void push(int x0, int y0, int x1, int y1, uint16_t *pixels, void *ctx) {
    (void)ctx;
    assert(x0 == 0 && x1 == R_W && y0 == pushed && y1 > y0 && y1 <= R_H);
    assert(!((uintptr_t)pixels & 3));
    assert(canvas_rows(&actual_diff, y0, y1, pixels));
    for (int y = y0; y < y1; y++) {
        assert(visits[y]++ == 0);
        memcpy(captured[y], pixels + (y - y0) * R_W, sizeof captured[y]);
        assert(!memcmp(captured[y], expected[y], sizeof captured[y]));
    }
    pushed = y1;
    check_guards();
}
static void ready(int y0, const uint16_t *band, void *ctx) {
    (void)ctx;
    assert(y0 == ready_count * R_BAND && band == guarded.workspace.band);
    assert(!memcmp(band, expected[y0], R_W * R_BAND * 2));
    int next = y0 + R_BAND;
    assert(pushed == (next < R_H ? next + 1 : R_H));
    if (next < R_H) {
        assert(visits[next] == 1);
        assert(!memcmp(guarded.workspace.lookahead, expected[next], R_W * 2));
        assert(actual_diff.changed_mask[next] == expected_diff.changed_mask[next]);
    }
    for (int y = y0; y < next; y++) {
        assert(visits[y] == 1);
        assert(actual_diff.changed_mask[y] == expected_diff.changed_mask[y]);
    }
    ready_count++;
    check_guards();
}
static void build(int kind) {
    scene_begin(&scene, kind == 1 ? 0x4279 : 0);
    if (kind >= 2) sc_image(&scene, 0, 0, 0, 0, 0, UI_W - 1, UI_H - 1, 1);
    if (kind == 3) {
        prim_t *clip = sc_mask_rbox(&scene, 80, 24, 55, 12, 4, .17f);
        prim_t *box = sc_rbox(&scene, 66, 16, 55, 10, 5, -.23f, 0xFF4A17, .7f);
        pr_clip(&scene, box, clip);
        sc_ring(&scene, 210, 31, 22, 3, 0x20CFFF, .63f);
    }
    if (kind == 4) {
        float radius[8] = {72, 68, 75, 66, 72, 68, 75, 66};
        scene_glass_begin(&scene, 150, 80, radius, 8);
        sc_rbox(&scene, 140, 90, 110, 70, 15, .14f, 0xFF8A12, .8f);
        sc_circle(&scene, 74, 16, 20, 0x70FFE0, .6f);
        scene_glass_end(&scene);
    }
    if (kind == 5) {
        for (int y = 0; y < UI_H; y += 17)
            sc_glow_dot(&scene, (float)(y * 13 % UI_W), (float)y, 3.5f, 0x7AFF93, 220);
        sc_capsule(&scene, -5, 13, 215, 18, 4, 0xFA5B21, .8f);
    }
}
static void compare_frame(int kind, bool swap, bool reset) {
    build(kind);
    scene_clone(&oracle, &scene);
    static uint16_t band[R_W * R_BAND];
    count_image = false;
    render_raster_prepare(&oracle, swap);
    for (int y = 0; y < R_H; y += R_BAND) {
        render_raster_window(&oracle, band, 0, R_W - 1, y, y + R_BAND - 1);
        memcpy(expected[y], band, sizeof band);
    }
    if (reset) { canvas_reset(&actual_diff); canvas_reset(&expected_diff); }
    assert(canvas_rows(&expected_diff, 0, R_H, &expected[0][0]));
    memset(visits, 0, sizeof visits);
    memset(image_visits, 0, sizeof image_visits);
    memset(&guarded.workspace, 0xD5, sizeof guarded.workspace);
    for (int i = 0; i < 4; i++) { guarded.before[i] = 0x93BAD01F; guarded.after[i] = 0x125AC37E; }
    pushed = ready_count = 0;
    count_image = true;
    render_state_t state = {0};
    render_frame(&state, &scene, &guarded.workspace, swap, push, ready, NULL);
    assert(pushed == R_H && ready_count == R_BANDS && state.pixels_last == R_W * R_H);
    assert(!memcmp(captured, expected, sizeof captured));
    for (int y = 0; y < R_H; y++) {
        assert(visits[y] == 1);
        assert(image_visits[y] == (unsigned)(kind >= 2));
    }
    scene_end(&scene); scene_end(&oracle);
}
int main(void) {
    render_set_image_fn(image);
    for (int swap = 0; swap < 2; swap++) {
        for (int kind = 0; kind < 6; kind++) {
            compare_frame(kind, swap, kind == 0);
            compare_frame(kind, swap, false); // unchanged frame, including next-row mask=0
        }
        compare_frame(3, swap, true); // explicit canvas reset across row 7/8
    }
    puts("rolling workspace: exact size/canaries, every row once, next-row hash before ready, independent full-band pixels");
    return 0;
}
