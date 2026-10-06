// Edge band overlay: the table-driven corners must match the per-pixel reference exactly.
// cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/render.h"
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int old_disc_cover(int x, int y, int cx, int cy, int r) {
    if (r <= 0) return 0;
    int dx = 2 * (x - cx) + 1, dy = 2 * (y - cy) + 1;  // doubled: pixel centres
    int v = 16 + (4 * r * r - (dx * dx + dy * dy)) * 4 / r;
    return v < 0 ? 0 : v > 32 ? 32 : v;
}

static void old_corner_overlay(const scene_t *s, uint16_t *row, int x0, int x1, int y, int cx, int cy, int r, int w, bool swap) {
    for (int x = x0; x < x1; x++) {
        int a = old_disc_cover(x, y, cx, cy, r) - old_disc_cover(x, y, cx, cy, r - w);
        if (a <= 0) continue;
        if (a >= 32) row[x] = swap ? rgb565_bswap(s->edge_color) : s->edge_color;
        else if (!swap) row[x] = rgb565_blend(s->edge_color, row[x], (uint32_t)a);
        else row[x] = rgb565_bswap(rgb565_blend(s->edge_color, rgb565_bswap(row[x]), (uint32_t)a));
    }
}

static void old_corner_pair(const scene_t *s, uint16_t *row, int xa, int xb, int y, int cy, int width, int r, int w, bool swap) {
    old_corner_overlay(s, row, xa, imin(xb, r), y, r, cy, r, w, swap);
    old_corner_overlay(s, row, imax(xa, width - r), xb, y, width - r, cy, r, w, swap);
}

static void old_edge_overlay(const scene_t *s, uint16_t *out, int x0, int y0, int width, int rows, bool swap) {
    int w = s->edge_w, r = s->edge_r;
    if (!w) return;
    const int W = R_W * R_SCALE, H = R_H * R_SCALE;
    if (r < w) r = w;
    uint16_t c = swap ? rgb565_bswap(s->edge_color) : s->edge_color;
    int xa = imax(x0, 0), xb = imin(x0 + width, W);
    for (int j = 0; j < rows; j++) {
        int y = y0 + j;
        uint16_t *row = out + j * width - x0;
        if (y >= r && y < H - r) {  // straight sides
            for (int x = xa; x < imin(xb, w); x++) row[x] = c;
            for (int x = imax(xa, W - w); x < xb; x++) row[x] = c;
            continue;
        }
        if (y < w || y >= H - w)  // straight top/bottom
            for (int x = imax(xa, r); x < imin(xb, W - r); x++) row[x] = c;
        int cy = y < r ? r : H - r;
        old_corner_pair(s, row, xa, xb, y, cy, W, r, w, swap);
    }
}


static uint16_t a[480 * 16], b[480 * 16];

// Whatever changes between two frames of the band (widths w1 -> w2, radius r) lies within the reach of the larger,
// in any 8 rows. (Asserts stay on in the sanitizer build.)
static void check_reach(void) {
    static scene_t s1, s2;
    long checked = 0;
    for (int w1 = 0; w1 <= 16; w1 += (w1 < 3 ? 1 : 5))
        for (int w2 = 0; w2 <= 16; w2++)
            for (int r = 1; r <= 110; r += 13)
                for (int y0 = 0; y0 + 8 <= 480; y0 += 4) {
                    memset(&s1, 0, sizeof s1); memset(&s2, 0, sizeof s2);
                    s1.edge_w = (uint8_t)w1; s2.edge_w = (uint8_t)w2; s1.edge_r = s2.edge_r = (uint8_t)r;
                    s1.edge_color = s2.edge_color = 0xF800;
                    for (int i = 0; i < 480 * 8; i++) a[i] = b[i] = 0x1234;
                    render_edge_overlay(&s1, a, 0, y0, 480, 8, false);
                    render_edge_overlay(&s2, b, 0, y0, 480, 8, false);
                    int reach = render_edge_reach(w1 > w2 ? w1 : w2, r, y0, y0 + 8);
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 480; x++)
                            if (a[y * 480 + x] != b[y * 480 + x]) assert(x < reach || x >= 480 - reach);
                    checked++;
                }
    printf("edge reach: %ld bands cover every changed pixel\n", checked);
}
// Does the change c (for rows y0..) name the pixel (x, y)?
static bool named(const edge_change_t *c, int x, int y) {
    bool side = x >= c->x0 && x < c->x1, mirror = 479 - x >= c->x0 && 479 - x < c->x1;
    bool strip = y >= c->sy0 && y < c->sy1 && x >= c->r && x < 480 - c->r;
    return side || mirror || strip;
}
// One case: a band of width w1 against one of w2 (same radius r) in rows y0..y0+rows; every pixel that differs must be
// named. Returns the area of the change, and adds the area of the reach to *reach_px.
static long check_one(int w1, int w2, int r, int y0, int rows, long *reach_px) {
    static scene_t s1, s2;
    memset(&s1, 0, sizeof s1); memset(&s2, 0, sizeof s2);
    s1.edge_w = (uint8_t)w1; s2.edge_w = (uint8_t)w2; s1.edge_r = s2.edge_r = (uint8_t)r;
    s1.edge_color = s2.edge_color = 0xF800;
    for (int i = 0; i < 480 * rows; i++) a[i] = b[i] = 0x1234;
    render_edge_overlay(&s1, a, 0, y0, 480, rows, false);
    render_edge_overlay(&s2, b, 0, y0, 480, rows, false);
    edge_change_t c = render_edge_change(w1, w2, r, y0, y0 + rows);
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < 480; x++)
            if (a[y * 480 + x] != b[y * 480 + x] && !named(&c, x, y0 + y)) {
                printf("pixel %d,%d missed: w %d -> %d r %d rows %d..%d hull %d..%d strip %d..%d\n", x, y0 + y, w1, w2, r, y0, y0 + rows, c.x0, c.x1, c.sy0, c.sy1);
                exit(1);
            }
    *reach_px += 2L * render_edge_reach(w1 > w2 ? w1 : w2, r, y0, y0 + rows) * rows;
    return (c.x1 > c.x0 ? 2L * (c.x1 - c.x0) * rows : 0) + (c.sy1 > c.sy0 ? (long)(c.sy1 - c.sy0) * (480 - 2 * c.r) : 0);
}
// Whatever differs between a band of width w1 and one of width w2 (same colour and radius; 0 = none) in any rows lies
// within what render_edge_change names, and that is a small part of the reach (its area is reported).
static void check_change(void) {
    long checked = 0, ring_px = 0, reach_px = 0;
    static const int radii[] = {1, 14, 30, 84, 110, 128, 200};
    for (int w1 = 0; w1 <= 24; w1++)
        for (int w2 = 0; w2 <= 24; w2++)
            for (size_t ri = 0; ri < sizeof radii / sizeof *radii; ri++)
                for (int y0 = 0; y0 + 16 <= 480; y0 += (y0 < 120 || y0 > 344 ? 2 : 37)) {
                    int r = radii[ri];
                    if (w1 && w2 && render_edge_radius(w1, r) != render_edge_radius(w2, r)) continue;  // a band grown past its radius
                    ring_px += check_one(w1, w2, r, y0, y0 % 3 == 0 ? 16 : 8, &reach_px);
                    checked++;
                }
    printf("edge change: %ld bands cover every changed pixel, %.1f%% of the area of their reach\n", checked, 100. * ring_px / reach_px);
}
int main(void) {
    static scene_t s;
    srand(3);
    long n = 0;
    for (int w = 1; w <= 20; w++)
        for (int iter = 0; iter < 400; iter++) {
            memset(&s, 0, sizeof s);
            s.edge_w = (uint8_t)w; s.edge_r = (uint8_t)(iter % 5 == 0 ? 84 : 20 + rand() % 100);
            s.edge_color = (uint16_t)rand();
            int x0 = (rand() % 240) * 2, width = 2 + 2 * (rand() % ((480 - x0) / 2)), rows = 1 + rand() % 8;
            int y0 = rand() % (481 - rows);
            if (iter % 7 == 0) { x0 = 0; width = 480; }
            bool swap = rand() & 1;
            for (int i = 0; i < width * rows; i++) a[i] = b[i] = (uint16_t)rand();
            old_edge_overlay(&s, a, x0, y0, width, rows, swap);
            render_edge_overlay(&s, b, x0, y0, width, rows, swap);
            if (memcmp(a, b, (size_t)width * rows * 2)) { printf("mismatch w=%d r=%d x0=%d y0=%d width=%d rows=%d\n", w, s.edge_r, x0, y0, width, rows); return 1; }
            n++;
        }
    printf("edge overlay: %ld windows match the per-pixel reference\n", n);
    check_reach();
    check_change();
}
