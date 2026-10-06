#pragma once

#include "render.h"
#include "character.h"

extern float s_extent[R_MAX_PRIMS];
extern uint32_t image_us, shapes_us, glass_us;

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
static inline void render_glass_snapshot(uint16_t *buf, int width) { (void)buf; (void)width; }
static inline void render_glass_restore(uint16_t *buf, int x0, int width, int y0, int y1) {
    (void)buf; (void)x0; (void)width; (void)y0; (void)y1;
}
#else
#define RENDER_GLASS_ENABLED 1
void render_glass_prepare(const scene_t *s);
bool render_glass_intersects(int x0, int x1, int y0, int y1);
void render_glass_snapshot(uint16_t *buf, int width);
void render_glass_restore(uint16_t *buf, int x0, int width, int y0, int y1);
#endif
