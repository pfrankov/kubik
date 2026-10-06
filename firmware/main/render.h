// Integer signed-distance rasteriser for a 480x480 RGB565 panel.
//
// The ESP32-C6 has no FPU, so per-pixel work is fixed point (Q4 = 1/16 px).
// Shapes are signed distance fields; each row is marched: far outside the
// shape whole runs are skipped, deep inside runs are filled, and only the
// antialiased edge is evaluated per pixel. Work is proportional to shape
// perimeter, not area.
//
// Scenes use 480px design coordinates, then compose at native sprite resolution
// (240px) into a lossless packed frame. Only completed frames reach the panel.
#pragma once

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define R_HOT IRAM_ATTR  // inner raster loops: keep them out of the flash cache the sprites stream through
#else
#define R_HOT
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "font.h"
#include "icons.h"

#define R_FPS 30
#define R_W 240
#define R_H 240
#define R_SCALE 2
#define R_BAND 8
#define R_BANDS (R_H / R_BAND)
#define R_MAX_PRIMS 80  // face stress maximum 48, text card 14; keep spare slots

enum { PK_RBOX = 0, PK_RING, PK_ARC, PK_HEART, PK_SPRITE, PK_QR, PK_TEXT, PK_ICON };
#define R_TEXT_POOL 640  // glyph indices of all text primitives in a scene

typedef struct {
    uint8_t kind;
    uint8_t alpha;       // 0..255
    uint16_t color;      // RGB565, native byte order
    int16_t soft;        // edge width, Q4 (16 = 1 px antialias)
    int16_t cs, sn;      // rotation, Q14 (local = R^T * (p - c))
    int32_t cx, cy;      // centre, Q4
    int32_t a, b, r;     // RBOX: half size a,b + corner r; RING: radius a, half width r;
                         // ARC: radius a, half width r; HEART: size a
    int16_t ap_s, ap_c;  // ARC half aperture sin/cos, Q14 (arc centred on local +y)
    uint8_t ncut;        // half-plane cuts in local space: keep n.p - c < 0
    int16_t cut_nx[2], cut_ny[2];
    int32_t cut_c[2];
    uint8_t has_sub;     // subtract a circle (local space)
    int32_t sub_x, sub_y, sub_r;
    int32_t stroke;      // >0: only a band this wide (Q4) inside the edge is kept (an outline)
    int8_t clip;         // intersect with the SDF of another primitive (index) or -1
    int16_t bx0, by0, bx1, by1;  // bounding box in pixels (inclusive)
} prim_t;

#define R_MAX_DOTS 160  // tesseract + recording orbit + touch wave
typedef struct { int16_t x, y, radius; uint16_t color; uint8_t glow, reserved; } render_dot_t;
typedef struct {
    render_dot_t dots[R_MAX_DOTS];  // native Q4, an independent background layer
    int dots_n;
    prim_t p[R_MAX_PRIMS];
    int n;
    bool raster_ready;
    uint16_t bg;
    // Optional transform applied by the builders: a homography from design to
    // panel space. Lets the face be designed in its own 480x480 space and placed
    // on the character's face screen, which moves (and turns) with the body.
    // Each primitive maps its centre exactly; its sizes and rotation follow the
    // local derivative there (exact for similarities, first order for perspective).
    bool xf_on;
    float xf_h[9];
    float xf_s, xf_sx, xf_sy, xf_px, xf_py, xf_pc, xf_ps;  // the primitive being built (pc, ps: its angle's cos, sin)
    float xf_alpha;  // alpha multiplier while the transform is on
    int8_t xf_clip;  // clip every new primitive to this one (-1 = none)
    // Primitives [glass_a, glass_b) only show inside the glass outline; elsewhere the video
    // shows through. -1 = off.
    int8_t glass_a, glass_b;
    int8_t glass_n;  // outline vertices (0 = no outline, no clipping)
    float glass_cx, glass_cy, glass_r[24];  // the glass as radii around a centre, panel px
    const uint8_t *qr;  // PK_QR modules, n*n bytes (nonzero = dark); one code per scene
    uint8_t text[R_TEXT_POOL];  // PK_TEXT glyphs (copied, so strings need not outlive the call)
    int text_n;
    // A band along the panel's rounded edge (sc_edge), drawn like text on the upscaled rows: a static
    // band never enters the canvas diff and costs no rasterising. 0 = none.
    uint8_t edge_w, edge_r;  // band width and corner radius, panel px
    uint16_t edge_color;
} scene_t;

// Float builders (a few dozen calls per frame; soft-float cost is negligible).
void scene_begin(scene_t *s, uint16_t bg);
void sc_glow_dot(scene_t *s, float x, float y, float radius, uint32_t rgb, uint8_t glow);
// The same dot already in 1/8 panel px (Tess projects its points on integers).
void sc_glow_dot_q3(scene_t *s, int x, int y, int radius, uint32_t rgb, uint8_t glow);
prim_t *sc_rbox(scene_t *s, float cx, float cy, float hw, float hh, float r, float angle, uint32_t rgb, float alpha);
prim_t *sc_circle(scene_t *s, float cx, float cy, float r, uint32_t rgb, float alpha);
prim_t *sc_capsule(scene_t *s, float x0, float y0, float x1, float y1, float thick, uint32_t rgb, float alpha);
prim_t *sc_ring(scene_t *s, float cx, float cy, float radius, float thick, uint32_t rgb, float alpha);
// Arc centred on the local +y axis (screen down) = a "U"; rotate by pi for "∩".
prim_t *sc_arc(scene_t *s, float cx, float cy, float radius, float half_aperture, float thick, float angle, uint32_t rgb,
               float alpha);
prim_t *sc_heart(scene_t *s, float cx, float cy, float size, float angle, uint32_t rgb, float alpha);
// Invisible rounded box: only used as a clip shape.
prim_t *sc_mask_rbox(scene_t *s, float cx, float cy, float hw, float hh, float r, float angle);
// Image primitive rasterised by the registered sprite hook; `id` selects the image,
// (x, y) is its origin in panel pixels, bbox is inclusive panel pixels.
prim_t *sc_image(scene_t *s, uint32_t id, int x, int y, int bx0, int by0, int bx1, int by1, float alpha);

// QR code: n*n modules (row-major bytes, nonzero = dark), each `module` native
// pixels square, centred on (cx, cy) design px. Axis aligned and pixel exact on
// the native grid so phone cameras read it; `mods` must outlive the frame.
prim_t *sc_qr(scene_t *s, const uint8_t *mods, int n, int module, float cx, float cy, uint32_t rgb, float alpha);

// One line of text (UTF-8; FONT_* from font.h) with its baseline at `y`,
// aligned by `align` (-1 left edge at x, 0 centred on x, 1 right edge at x),
// with `spacing` extra px between letters (tracking for capitals).
// Text is not part of the native canvas: render_text_overlay draws it on the
// upscaled panel rows at full resolution, so it stays sharp. The transform is ignored.
prim_t *sc_text(scene_t *s, int font, const char *utf8, float x, float y, int align, float spacing, uint32_t rgb,
                float alpha);
// A pinned Lucide icon, rasterized to a small alpha mask at build time. Like
// text, icons are composited on the 480px output after native canvas expansion.
prim_t *sc_icon(scene_t *s, int icon, float x, float y, float size, uint32_t rgb, float alpha);
float text_width(int font, const char *utf8, float spacing);  // design (= panel) px
// Text set by a role (font.h): the middle of its capitals on `cy`, so labels centre the same way in every font.
prim_t *sc_label(scene_t *s, text_role_t role, const char *utf8, float x, float cy, int align, uint32_t rgb, float alpha);
float label_width(text_role_t role, const char *utf8);
// Longest whole UTF-8 prefix within both limits, including suffix width and kerning.
size_t label_prefix(text_role_t role, const char *utf8, size_t max_bytes, float width, const char *suffix);
// Draws the scene's text into panel rows y0..y0+rows of a window starting at
// column x0, `width` wide (after render_expand_2x on the same rows).
void render_text_overlay(const scene_t *s, uint16_t *out, int x0, int y0, int width, int rows, bool swap);
// Signature of everything the text overlay draws, and the native-pixel area it
// covers (exclusive end; empty when there is no text), for renderer diagnostics.
// Presentation uses individual overlay items to bound the actual dirty region.
// Full-resolution text/icon state, with bounds in the native diff coordinate space.
typedef struct { uint32_t signature; int16_t x0, y0, x1, y1; } render_overlay_item_t;
bool render_overlay_item(const scene_t *s, const prim_t *p, render_overlay_item_t *item);
uint32_t render_text_signature(const scene_t *s, int *x0, int *y0, int *x1, int *y1);

// Band of width `w` inside the panel's rounded edge (corner radius `r`), panel px.
void sc_edge(scene_t *s, int w, int r, uint32_t rgb);
// Draws the edge band into panel rows like render_text_overlay (call it before that).
void render_edge_overlay(const scene_t *s, uint16_t *out, int x0, int y0, int width, int rows, bool swap);
// The columns [0, n) and [W - n, W) of panel rows [y0, y1) that an edge band of width w and corner radius r can
// touch: n is the panel width when the rows cross the top or bottom of the band, 0 when there is no band.
int render_edge_reach(int w, int r, int y0, int y1);
// The radius an edge band of width w is drawn with: r, at least w, at most what the tables hold.
int render_edge_radius(int w, int r);
// Where the panel rows [y0, y1) (one band) can look different when an edge band of width w0 is replaced by one of
// width w1 (none: 0) with the same colour and radius r: the columns [x0, x1) of the left corner or side (the right
// one is the mirror image; none when x0 >= x1), and the rows [sy0, sy1) of the straight top or bottom between the
// corners, columns [r, width - r) (none when sy0 >= sy1). Only pixels the two bands colour differently are in it.
typedef struct { int16_t x0, x1, sy0, sy1, r; } edge_change_t;
edge_change_t render_edge_change(int w0, int w1, int r, int y0, int y1);
// Changes whenever the band looks different; it then covers the whole panel.
static inline uint32_t render_edge_signature(const scene_t *s) {
    return s->edge_w ? (uint32_t)s->edge_w << 24 | (uint32_t)s->edge_r << 16 | s->edge_color : 0;
}

// Builder transform (see scene_t). Sizes scale by `scale`, angles add `angle`.
void scene_xform(scene_t *s, float ox, float oy, float tx, float ty, float scale, float angle);
// Maps the design rectangle (ox±hw, oy±hh) onto quad (TL, TR, BR, BL; x,y pairs).
void scene_xform_quad(scene_t *s, float ox, float oy, float hw, float hh, const float quad[8]);
void scene_xform_off(scene_t *s);
// Everything added until scene_glass_end shows only on the glass: inside the outline given by
// radii r[0..n) at 360/n degree steps from +x, clockwise on screen (NULL = no clipping). The
// outline is filled row by row between its outermost edges.
void scene_glass_begin(scene_t *s, float cx, float cy, const float *r, int n);
void scene_glass_end(scene_t *s);
void scene_clip_all(scene_t *s, const prim_t *clip);  // NULL = off

// Modifiers (apply to the primitive just created, before the next one).
void pr_soft(prim_t *p, float px);                         // wider edge = glow
void pr_cut(prim_t *p, float nx, float ny, float c);       // local half-plane, keeps n.p < c
void pr_sub_circle(prim_t *p, float lx, float ly, float r);  // local-space subtraction
void pr_stroke(prim_t *p, float width);  // outline: keep only a `width` px band inside the edge
void pr_clip(scene_t *s, prim_t *p, const prim_t *clip);

static inline uint16_t rgb565(uint32_t rgb) {
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}
uint32_t rgb_mix(uint32_t a, uint32_t b, float t);
uint32_t rgb_scale(uint32_t a, float k);

static inline uint16_t rgb565_blend(uint16_t fg, uint16_t bg, uint32_t a) {  // a: 0..32
    uint32_t f = (fg | ((uint32_t)fg << 16)) & 0x07E0F81F;
    uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81F;
    b += ((f - b) * a) >> 5;
    b &= 0x07E0F81F;
    return (uint16_t)(b | (b >> 16));
}

// Rasterises rows ya..yb of an image primitive into buf (band starting at band_y0,
// window starting at column bx0, bw wide).
typedef void (*render_image_fn)(const prim_t *p, uint16_t *buf, int bx0, int bw, int ya, int yb, int band_y0);
void render_set_image_fn(render_image_fn fn);
// True while an image primitive is the first thing drawn into a band cleared to black:
// the image may then write its transparent (zero) pixels as-is instead of testing them.
extern bool g_render_img_opaque_ok;
// True while rendering for the panel: bands hold byte-swapped (big-endian) RGB565.
extern bool g_render_swap;
static inline uint16_t rgb565_bswap(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
// Two pixels as one 32-bit word (the pointer must be 4-byte aligned): pairs move in half the steps.
static inline uint32_t *pair_at(uint16_t *q) { return (uint32_t *)(void *)q; }
static inline uint32_t pair_of(uint16_t v) { return v | ((uint32_t)v << 16); }

typedef void (*render_push_fn)(int x0, int y0, int x1, int y1, uint16_t *px, void *ctx);

typedef struct {
    uint32_t image_us, shapes_us, prep_us, glass_us;
    uint32_t pixels_last;
} render_state_t;

// Compose the entire 240px canvas. Dirty output is determined from the completed
// pixels by canvas.c, so primitive metadata never races a visible panel update.
void render_frame(render_state_t *st, const scene_t *s, uint16_t **bufs, int nbufs, bool swap, render_push_fn push,
                  void *ctx);

// Cached bilinear 2x expansion of a completed native canvas. The row reader
// allows the device to keep its frame losslessly packed instead of allocating
// a second full-resolution framebuffer. It is asked for the pixels [xa, xb) of row y and may fill more, but not less.
typedef void (*render_row_fn)(void *ctx, int y, uint16_t *row, int xa, int xb);
typedef struct {
    render_row_fn read;
    void *ctx;
    int key[2], source_key[2], xa, xb;
    bool swap[2];
    uint32_t row[2][R_W];   // native pixel | midpoint with the next one << 16
    uint32_t even[2][R_W];  // the same pairs in the output byte order: an even panel row as is
    uint16_t source[2][R_W + 2];  // [R_W] repeats the last pixel
} render_output_t;
void render_output_begin(render_output_t *st, render_row_fn read, void *ctx);
// A window of the panel (px, end exclusive, even).
typedef struct { int16_t x0, y0, x1, y1; } render_win_t;
// The windows of a band of native pairs p0..p1-1 from the changed stripes of their rows (canvas.h): a run of stripes
// each, stripes one apart joined. At most one per two stripes; returns how many.
#define RENDER_BAND_WINDOWS 8
int render_band_windows(const uint16_t *mask, int p0, int p1, render_win_t *out);
// Optional: only panel columns [x0, x1) will be expanded until the next call (rows stay decoded).
void render_output_columns(render_output_t *st, int x0, int x1);
// Output is 4-byte aligned; panel coordinates, width and row count are even.
void render_expand_2x(render_output_t *st, uint16_t *out, int x0, int y0, int width, int rows, bool swap);
