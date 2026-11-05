#include "render_projection.h"

#define Q 16
static int larger(int a, int b) { return a > b ? a : b; }

static void project_source_origin(prim_t *p) {
#if CONFIG_IDF_TARGET_ESP32S3
    // QR sub_x/sub_y (origin) and b (square side) use physical output pixels.
    if (p->kind == PK_SPRITE) {
        p->sub_x = p->cx; p->sub_r = p->cy; // sprite source grid stays 240x240
    }
#else
    (void)p;
#endif
}
static void project_primitive(prim_t *p) {
    project_source_origin(p);
    p->cx = ui_canvas(p->cx);
    p->cy = ui_canvas(p->cy);
    if (p->kind >= PK_SPRITE) {
        p->bx0 = (int16_t)ui_canvas(p->bx0);
        p->by0 = (int16_t)ui_canvas(p->by0);
        p->bx1 = (int16_t)ui_canvas(p->bx1);
        p->by1 = (int16_t)ui_canvas(p->by1);
        return; // source font/icon/QR/sprite parameters are not canvas dimensions
    }
    p->a = ui_canvas(p->a);
    p->b = ui_canvas(p->b);
    p->r = ui_canvas(p->r);
    if (p->kind == PK_HEART) {
        p->b = (p->a * 55) / 100;
        p->r = (p->a * 3536) / 10000;
    }
    p->soft = (int16_t)larger(Q, ui_canvas(p->soft));
    p->sub_x = ui_canvas(p->sub_x);
    p->sub_y = ui_canvas(p->sub_y);
    p->sub_r = ui_canvas(p->sub_r);
    if (p->stroke) p->stroke = larger(Q / 2, ui_canvas(p->stroke));
    for (int j = 0; j < p->ncut; j++) p->cut_c[j] = ui_canvas(p->cut_c[j]);
}
void render_project_scene(scene_t *s) {
    if (s->raster_ready) return;
    s->raster_ready = true;
    for (int i = 0; i < s->n; i++) project_primitive(&s->p[i]);
#if CONFIG_IDF_TARGET_ESP32S3
    // Tess supplies design Q3; the raster stores half-resolution Q4.
    for (int i = 0; i < s->dots_n; i++) {
        s->dots[i].x = (int16_t)ui_output(s->dots[i].x);
        s->dots[i].y = (int16_t)ui_output(s->dots[i].y);
        s->dots[i].radius = (int16_t)ui_output(s->dots[i].radius);
    }
#endif
}
