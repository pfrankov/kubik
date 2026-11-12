#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#define UI_W 480
#define MODE_SETUP 1
#define MENU_SWAP .5f

typedef struct { bool screen; } printer_preview_t;
typedef struct { int b; } prim_t;
typedef struct { printer_preview_t *printer_preview; } scene_t;
static struct { int mode; bool live_active; struct { bool open; float k; } menu; } g_face;
static printer_preview_t image;
static prim_t primitive;
static int x0, y0, x1, y1, begins, ends, releases, images;
static uint32_t link_session(void) { return 1; }
static printer_preview_t *printer_preview_snapshot(uint32_t session) { assert(session == 1); return &image; }
static bool printer_preview_is_screen(const printer_preview_t *p) { return p->screen; }
static bool printer_preview_draw_allowed(const printer_preview_t *p, bool open, float k) { return !p->screen || (!open && k <= 0); }
static void printer_preview_release(printer_preview_t *p) { assert(p == &image); releases++; }
static void scene_end(scene_t *s) { (void)s; ends++; }
static void scene_begin(scene_t *s, uint16_t bg) { (void)s; assert(bg == 0); begins++; }
static uint32_t printer_preview_hash(const printer_preview_t *p) { assert(p == &image); return 123; }
static prim_t *sc_image(scene_t *s, uint32_t id, int x, int y, int a, int b, int c, int d, float alpha) {
    assert(s->printer_preview == &image && id == UINT32_MAX && x == a && y == b && alpha == 1.f);
    x0 = a; y0 = b; x1 = c; y1 = d; images++; return &primitive;
}
/* PRODUCTION_DRAW_PREVIEW */
static void check(bool screen, bool live, int side, bool replaces) {
    scene_t scene = {0}; image.screen = screen; g_face.live_active = live;
    begins = ends = releases = images = 0; primitive.b = 0;
    draw_preview(&scene);
    assert(images == 1 && primitive.b == 123 && scene.printer_preview == &image);
    assert(x1 - x0 + 1 == side && y1 - y0 + 1 == side);
    assert(begins == replaces && ends == replaces && releases == 0);
    if (screen && live) assert(x0 == UI_W - 170 && y0 == 90);
    else assert(x0 == (UI_W - side) / 2 && y0 == (UI_H - side) / 2);
}
int main(void) {
    int centered = UI_H == 480 ? 256 : 336;
    check(true, false, centered, true);
    check(false, false, centered, true);
    check(true, true, 112, false);
    check(false, true, centered, true);
    g_face.menu.open = true;
    scene_t scene = {0}; image.screen = true; images = releases = 0;
    draw_preview(&scene); assert(!images && releases == 1 && !scene.printer_preview);
    g_face.menu.open = false; g_face.mode = MODE_SETUP; images = releases = 0;
    draw_preview(&scene); assert(!images && !releases && !scene.printer_preview);
    puts("screen image: C6/S3 centered geometry, Live, print, menu and setup passed");
}
