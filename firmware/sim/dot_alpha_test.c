// The pre-optimization scalar dot raster is the pixel oracle, including background blending.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/render_dots.c"
bool g_render_swap;
static inline R_HOT void reference_dot(const render_dot_t *p, uint32_t halo_step, uint16_t *buf,
                                   int x0, int x1, int y0, int y1, bool swap) {
    int width = x1 - x0 + 1;
    int r = p->radius, core = r + 8, outer = p->glow ? r * 3 + 16 : core;
    int ya = imax(y0, (p->y - outer) >> 4), yb = imin(y1, (p->y + outer) >> 4);
    if (ya > yb) return;
    int xa = imax(x0, (p->x - outer) >> 4), xb = imin(x1, (p->x + outer) >> 4);
    int rr = outer * outer, cr = core * core, core_full = r * 32;
    // floor(inside / r) as a multiply: exact for inside < 32 r while r < 1448 (Q4).
    uint32_t core_step = (1u << 26) / (uint32_t)imax(1, r) + 1;
    for (int y = ya; y <= yb; y++) {
        int dy = y * 16 + 8 - p->y;
        int dy2 = dy * dy;
        if (dy2 >= rr) continue;
        uint16_t *row = buf + (y - y0) * width;
        for (int x = xa; x <= xb; x++) {
            int dx = x * 16 + 8 - p->x;
            int distance = dx * dx + dy2;
            int coverage = rr - distance;
            if (coverage <= 0) continue;
            // Bounded soft halo (coverage * 18 / rr), plus an opaque antialiased core. Integer-only.
            int halo = (int)(((uint64_t)(uint32_t)coverage * halo_step) >> 32);
            halo = ((halo * halo / 18) * (p->glow + 1)) >> 8;
            int inside = cr - distance, a = halo;
            if (!p->blur && inside > 0) a = imax(a, inside >= core_full ? 32 : (int)(((uint32_t)inside * core_step) >> 26));
            if (!a) continue;
            uint16_t background = row[x - x0];
            if (swap) background = rgb565_bswap(background);
            uint16_t color = rgb565_blend(p->color, background, (unsigned)a);
            row[x - x0] = swap ? rgb565_bswap(color) : color;
        }
    }
}

static uint32_t random_word(uint32_t *seed) { *seed = *seed * 1664525u + 1013904223u; return *seed; }

static void clipped_rasters(void) {
    scene_t scene = {.dots_n = 1};
    uint16_t before[64 * R_BAND], after[64 * R_BAND];
    uint32_t seed = 0x64f2a195;
    for (int trial = 0; trial < 5000; trial++) {
        int x0 = (int)(random_word(&seed) % (R_W - 64)), y0 = (int)(random_word(&seed) % R_BANDS) * R_BAND;
        scene.dots[0] = (render_dot_t){.x = (int16_t)((int)(random_word(&seed) % 4608) - 384),
            .y = (int16_t)((int)(random_word(&seed) % 4608) - 384),
            .radius = (int16_t)(random_word(&seed) % 1448),
            .color = (uint16_t)random_word(&seed), .glow = (uint8_t)random_word(&seed), .blur = trial & 1};
        for (int i = 0; i < 64 * R_BAND; i++) before[i] = i % 3 ? 0 : (uint16_t)random_word(&seed);
        memcpy(after, before, sizeof before);
        render_dots_prepare(&scene);
        reference_dot(&scene.dots[0], s_halo_step[0], before, x0, x0 + 63, y0, y0 + R_BAND - 1, trial & 1);
        raster_dot(&scene.dots[0], s_halo_step[0], s_paint_radius2[0], s_paint_radius[0], after,
                   x0, x0 + 63, y0, y0 + R_BAND - 1, trial & 1);
        assert(!memcmp(before, after, sizeof before));
    }
    for (int radius = 0; radius < 1448; radius++) {
        scene.dots[0].radius = radius; scene.dots[0].glow = 1;
        render_dots_prepare(&scene);
        uint64_t outer = (unsigned)(radius * 3 + 16);
        // Coverage never exceeds outer²; the maximum lookup index is at zero distance.
        assert(((outer * outer * s_halo_step[0]) >> 32) <= 18);
    }
    puts("dot spans: 5000 clipped/overlapping rasters pixel-exact; halo lookup bounded for every supported radius");
}

static void optical_blur_has_no_flat_core(void) {
    scene_t scene={.dots_n=1};
    scene.dots[0]=(render_dot_t){.x=32*16+8,.y=3*16+8,.radius=48,.color=0xFFFF,.glow=255,.blur=1};
    uint16_t pixels[64*R_BAND]={0};
    render_dots_prepare(&scene);
    raster_dot(&scene.dots[0],s_halo_step[0],s_paint_radius2[0],s_paint_radius[0],pixels,0,63,0,R_BAND-1,false);
    unsigned center=pixels[3*64+32], inner=pixels[3*64+34], middle=pixels[3*64+36], outer=pixels[3*64+38];
    assert(center<0xFFFF && center>inner && inner>middle && middle>outer && outer>0);
}

int main(void) {
    clipped_rasters();
    optical_blur_has_no_flat_core();
    scene_t scene = {0}; scene.dots_n = 1;
    uint16_t before[64 * R_BAND], after[64 * R_BAND];
    unsigned cases = 0;
    for (int radius = 0; radius <= 100; radius += 7)
        for (unsigned glow = 0; glow <= 255; glow++)
            for (int phase = 0; phase < 3; phase++)
                for (int swap = 0; swap < 2; swap++)
                    for (int bg = 0; bg < 3; bg++) {
                    scene.dots[0] = (render_dot_t){.x = 32 * 16 + (int)(glow & 15), .y = phase * 72 - 32,
                        .radius = radius, .color = (uint16_t)(0x74a5 ^ glow * 317), .glow = glow};
                    for (int i = 0; i < 64 * R_BAND; i++) before[i] = bg == 0 || (bg == 1 && i % 3) ? 0 : (uint16_t)(i * 37 + glow);
                    memcpy(after, before, sizeof before);
                    render_dots_prepare(&scene);
                    reference_dot(&scene.dots[0], s_halo_step[0], before, 0, 63, 0, R_BAND - 1, swap);
                    raster_dot(&scene.dots[0], s_halo_step[0], s_paint_radius2[0], s_paint_radius[0], after, 0, 63, 0, R_BAND - 1, swap);
                    assert(!memcmp(before, after, sizeof before)); cases++;
                }
    printf("dot alpha pruning: %u rasters pixel-exact against prior blending, all glow levels and byte orders\n", cases);
}
