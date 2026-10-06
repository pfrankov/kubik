#include "render_internal.h"

#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#define R_NOW() ((uint32_t)esp_timer_get_time())
#else
#define R_NOW() 0u
#endif

// Renders columns dx0..dx1 (dx0 even, width even) of rows y0..y1 into buf.
R_HOT void render_raster_window(const scene_t *s, uint16_t *buf, int dx0, int dx1, int y0, int y1) {
    int bw = dx1 - dx0 + 1;
    if (s->bg == 0) {
        memset(buf, 0, (size_t)bw * R_BAND * 2);
    } else {
        uint16_t bg = g_render_swap ? rgb565_bswap(s->bg) : s->bg;
        uint32_t w2 = bg | ((uint32_t)bg << 16), *b32 = (uint32_t *)buf;
        for (int i = 0; i < bw * R_BAND / 2; i++) b32[i] = w2;
    }
    uint32_t td = R_NOW();
    if (s->dots_n) render_raster_dots(s, buf, dx0, dx1, y0, y1);
    shapes_us += R_NOW() - td;
    bool drawn = s->dots_n > 0;
    bool glass = render_glass_intersects(dx0, dx1, y0, y1);
    for (int i = 0; i < s->n; i++) {
        const prim_t *p = &s->p[i];
        if (glass && i == s->glass_a) render_glass_snapshot(buf, bw);
        if (glass && i == s->glass_b) {
            uint32_t tg = R_NOW();
            render_glass_restore(buf, dx0, bw, y0, y1);
            glass = false;
            glass_us += R_NOW() - tg;
        }
        drawn = render_raster_primitive(s, p, buf, dx0, bw, y0, y1, drawn);
    }
    if (glass) {
        uint32_t tg = R_NOW();
        render_glass_restore(buf, dx0, bw, y0, y1);
        glass_us += R_NOW() - tg;
    }
}
