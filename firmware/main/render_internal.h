#pragma once

#include "render.h"
#include "character.h"

extern float s_extent[R_MAX_PRIMS];
extern uint32_t image_us, shapes_us, glass_us;
int render_token_advance(const font_t *font, uint16_t token);
int render_token_kern(const font_t *font, uint16_t first, uint16_t second);
prim_t *render_make_text(scene_t *s, const font_t *font, int font_id, int start, int count,
    float x, float y, int align, float spacing, uint32_t rgb, float alpha);
void render_emoji_overlay(const scene_t *s, uint16_t token, uint16_t *out,
    int x0, int y0, int width, int rows, int stride, int left, int top, uint8_t opacity, bool swap);

uint32_t render_raster_prepare(scene_t *s, bool swap);
void render_raster_window(const scene_t *s, uint16_t *buf, int x0, int x1, int y0, int y1);
void render_raster_get_timings(render_state_t *st);
void render_dots_prepare(const scene_t *s);
void render_raster_dots(const scene_t *s, uint16_t *buf, int x0, int x1, int y0, int y1);
bool render_raster_primitive(const scene_t *s, const prim_t *p, uint16_t *buf, int x0, int width, int y0, int y1,
                             bool drawn);

// Glass belongs to Plush's video body. The native simulator renders both; a
// Tess-only firmware has no glass primitives and does not need its 7.7 KiB BSS.
#if defined(ESP_PLATFORM) && KUBIK_CHARACTER == 1
#define RENDER_GLASS_ENABLED 0
static inline void render_glass_prepare(const scene_t *s) { (void)s; }
static inline bool render_glass_intersects(int x0, int x1, int y0, int y1) {
    (void)x0; (void)x1; (void)y0; (void)y1; return false;
}
static inline void render_glass_snapshot(uint16_t *buf, int width, int rows) { (void)buf; (void)width; (void)rows; }
static inline void render_glass_restore(uint16_t *buf, int x0, int width, int y0, int y1) {
    (void)buf; (void)x0; (void)width; (void)y0; (void)y1;
}
#else
#define RENDER_GLASS_ENABLED 1
void render_glass_prepare(const scene_t *s);
bool render_glass_intersects(int x0, int x1, int y0, int y1);
void render_glass_snapshot(uint16_t *buf, int width, int rows);
void render_glass_restore(uint16_t *buf, int x0, int width, int y0, int y1);
#endif
