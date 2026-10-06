#include "render_internal.h"

#include "font.h"

#include <math.h>
#include <string.h>

#define Q 16  // sub-pixel units per pixel

static inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

// ------------------------------------------------------------------ builders

void sc_glow_dot_q3(scene_t *s, int x, int y, int radius, uint32_t rgb, uint8_t glow) {
    if (s->dots_n >= R_MAX_DOTS) return;
    s->dots[s->dots_n++] = (render_dot_t){(int16_t)x, (int16_t)y, (int16_t)radius, rgb565(rgb), glow, 0};
}

void sc_glow_dot(scene_t *s, float x, float y, float radius, uint32_t rgb, uint8_t glow) {
    sc_glow_dot_q3(s, (int)lrintf(x * 8), (int)lrintf(y * 8), (int)lrintf(radius * 8), rgb, glow);
}

void scene_begin(scene_t *s, uint16_t bg) {
    s->dots_n = 0;
    s->n = 0;
    s->raster_ready = false;
    s->bg = bg;
    s->xf_on = false;
    s->xf_clip = -1;
    s->glass_a = s->glass_b = -1;
    s->glass_n = 0;
    s->text_n = 0;
    s->edge_w = 0;
}

void scene_xform(scene_t *s, float ox, float oy, float tx, float ty, float scale, float angle) {
    float c = scale * cosf(angle), n = scale * sinf(angle);
    const float h[9] = {c, -n, tx - (c * ox - n * oy), n, c, ty - (n * ox + c * oy), 0, 0, 1};
    memcpy(s->xf_h, h, sizeof h);
    s->xf_on = true;
    s->xf_alpha = 1.f;
}

void scene_xform_quad(scene_t *s, float ox, float oy, float hw, float hh, const float q[8]) {
    // Unit square -> quad (Heckbert), then design rectangle -> unit square.
    float x0 = q[0], y0 = q[1], x1 = q[2], y1 = q[3], x2 = q[4], y2 = q[5], x3 = q[6], y3 = q[7];
    float sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3, g = 0, k = 0;
    if (fabsf(sx) > 1e-4f || fabsf(sy) > 1e-4f) {
        float dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2, den = dx1 * dy2 - dx2 * dy1;
        if (fabsf(den) > 1e-6f) {
            g = (sx * dy2 - dx2 * sy) / den;
            k = (dx1 * sy - sx * dy1) / den;
        }
    }
    float a = x1 - x0 + g * x1, b = x3 - x0 + k * x3, d = y1 - y0 + g * y1, e = y3 - y0 + k * y3;
    float iu = 0.5f / hw, iv = 0.5f / hh, u0 = -(ox - hw) * iu, v0 = -(oy - hh) * iv;
    const float h[9] = {a * iu, b * iv, a * u0 + b * v0 + x0,
                        d * iu, e * iv, d * u0 + e * v0 + y0,
                        g * iu, k * iv, g * u0 + k * v0 + 1};
    memcpy(s->xf_h, h, sizeof h);
    s->xf_on = true;
    s->xf_alpha = 1.f;
}

// Places the primitive being built: centre, rotation and per-axis scale at (x, y).
// The builders take angles; c, n are the angle's cos and sin (no FPU: trigonometry is costly, and most
// primitives are upright).
static inline void angle_cs(float angle, float *c, float *n) {
    *c = 1.f; *n = 0.f;
    if (angle != 0.f) { *c = cosf(angle); *n = sinf(angle); }
}

static void xf_prep(scene_t *s, float x, float y, float c, float n) {
    const float *h = s->xf_h;
    float w = h[6] * x + h[7] * y + h[8];
    if (fabsf(w) < 1e-6f) w = 1e-6f;
    float X = (h[0] * x + h[1] * y + h[2]) / w, Y = (h[3] * x + h[4] * y + h[5]) / w;
    float j00 = (h[0] - X * h[6]) / w, j01 = (h[1] - X * h[7]) / w;
    float j10 = (h[3] - Y * h[6]) / w, j11 = (h[4] - Y * h[7]) / w;
    float ux = j00 * c + j01 * n, uy = j10 * c + j11 * n;
    float sx = sqrtf(ux * ux + uy * uy);
    if (sx < 1e-6f) sx = 1e-6f;
    s->xf_px = X;
    s->xf_py = Y;
    s->xf_pc = ux / sx;  // the mapped angle's cos and sin
    s->xf_ps = uy / sx;
    s->xf_sx = sx;
    s->xf_sy = fabsf(j00 * j11 - j01 * j10) / sx;
    s->xf_s = sqrtf(s->xf_sx * s->xf_sy);
}

void scene_xform_off(scene_t *s) { s->xf_on = false; }

void scene_glass_begin(scene_t *s, float cx, float cy, const float *r, int n) {
    s->glass_a = (int8_t)s->n;
    s->glass_b = -1;
    s->glass_n = r ? (int8_t)(n < 24 ? n : 24) : 0;
    s->glass_cx = cx;
    s->glass_cy = cy;
    for (int i = 0; i < s->glass_n; i++) s->glass_r[i] = r[i];
}
void scene_glass_end(scene_t *s) { s->glass_b = s->glass_a >= 0 && s->n > s->glass_a ? (int8_t)s->n : -1; }

void scene_clip_all(scene_t *s, const prim_t *clip) { s->xf_clip = clip ? (int8_t)(clip - s->p) : -1; }

float s_extent[R_MAX_PRIMS];  // conservative radius of each primitive, px (-1 axis box, -2 preset)
static float s_scale = 1.f;  // size scale of the primitive being built (for the pr_* modifiers)
static float s_sx = 1.f, s_sy = 1.f;  // ... along its local axes

static inline float SZ(const scene_t *s, float v) { return s->xf_on ? v * s->xf_s : v; }
static inline float SZX(const scene_t *s, float v) { return s->xf_on ? v * s->xf_sx : v; }
static inline float SZY(const scene_t *s, float v) { return s->xf_on ? v * s->xf_sy : v; }
#define XF_PREP_CS(s, x, y, c, n) do { if ((s)->xf_on) xf_prep((s), (x), (y), (c), (n)); } while (0)
#define XF_PREP(s, x, y, ang) do { if ((s)->xf_on) { float c_, n_; angle_cs((ang), &c_, &n_); xf_prep((s), (x), (y), c_, n_); } } while (0)

static prim_t *add_cs(scene_t *s, int kind, float cx, float cy, float c, float n, uint32_t rgb, float alpha) {
    if (s->n >= R_MAX_PRIMS) return NULL;
    if (s->xf_on) alpha *= s->xf_alpha;
    if (alpha <= 0.004f) return NULL;
    prim_t *p = &s->p[s->n++];
    memset(p, 0, sizeof(*p));
    s_scale = s_sx = s_sy = 1.f;
    if (s->xf_on) {  // the builder ran XF_PREP for this centre and angle
        cx = s->xf_px;
        cy = s->xf_py;
        c = s->xf_pc;
        n = s->xf_ps;
        s_scale = s->xf_s;
        s_sx = s->xf_sx;
        s_sy = s->xf_sy;
    }
    p->kind = kind;
    p->alpha = alpha >= 1.f ? 255 : (uint8_t)(alpha * 255.f);
    p->color = rgb565(rgb);
    p->soft = Q;
    p->cx = (int32_t)lrintf(cx * Q);
    p->cy = (int32_t)lrintf(cy * Q);
    p->cs = (int16_t)lrintf(c * 16383.f);
    p->sn = (int16_t)lrintf(n * 16383.f);
    if (n == 0.f && c > 0.f) {  // upright
        p->cs = 16384;
        p->sn = 0;
    }
    p->clip = s->xf_clip;
    return p;
}

static prim_t *add(scene_t *s, int kind, float cx, float cy, float angle, uint32_t rgb, float alpha) {
    float c = 1.f, n = 0.f;
    if (!s->xf_on) angle_cs(angle, &c, &n);  // else XF_PREP has mapped the angle
    return add_cs(s, kind, cx, cy, c, n, rgb, alpha);
}

static void set_extent(scene_t *s, prim_t *p, float ext) { s_extent[p - s->p] = ext; }

static prim_t *rbox_cs(scene_t *s, float cx, float cy, float hw, float hh, float r, float c, float n, uint32_t rgb,
                       float alpha) {
    XF_PREP_CS(s, cx, cy, c, n);
    hw = SZX(s, hw);
    hh = SZY(s, hh);
    r = SZ(s, r);
    if (hw < 0.5f || hh < 0.5f) return NULL;
    prim_t *p = add_cs(s, PK_RBOX, cx, cy, c, n, rgb, alpha);
    if (!p) return NULL;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    if (r < 0) r = 0;
    p->a = (int32_t)(hw * Q);
    p->b = (int32_t)(hh * Q);
    p->r = (int32_t)(r * Q);
    set_extent(s, p, (p->cs == 16384 && p->sn == 0) ? -1.f : sqrtf(hw * hw + hh * hh));
    return p;
}

prim_t *sc_rbox(scene_t *s, float cx, float cy, float hw, float hh, float r, float angle, uint32_t rgb, float alpha) {
    float c, n;
    angle_cs(angle, &c, &n);
    return rbox_cs(s, cx, cy, hw, hh, r, c, n, rgb, alpha);
}

prim_t *sc_mask_rbox(scene_t *s, float cx, float cy, float hw, float hh, float r, float angle) {
    prim_t *p = sc_rbox(s, cx, cy, hw, hh, r, angle, 0, 1.f);
    if (p) p->alpha = 0;  // never drawn, only clips
    return p;
}

prim_t *sc_qr(scene_t *s, const uint8_t *mods, int n, int module, float cx, float cy, uint32_t rgb, float alpha) {
    if (s->n >= R_MAX_PRIMS || alpha <= 0.004f || !mods || n <= 0 || module <= 0) return NULL;
    prim_t *p = &s->p[s->n++];
    memset(p, 0, sizeof(*p));
    p->kind = PK_QR;
    p->alpha = alpha >= 1.f ? 255 : (uint8_t)(alpha * 255.f);
    p->color = rgb565(rgb);
    p->a = n;
    p->b = module;
    int ox = (int)lrintf(cx / R_SCALE) - n * module / 2, oy = (int)lrintf(cy / R_SCALE) - n * module / 2;
    p->cx = ox * R_SCALE;  // design px until canvas_geometry, like image primitives
    p->cy = oy * R_SCALE;
    p->clip = -1;
    p->bx0 = (int16_t)imax(ox * R_SCALE, 0);
    p->by0 = (int16_t)imax(oy * R_SCALE, 0);
    p->bx1 = (int16_t)imin((ox + n * module) * R_SCALE - 1, R_W * R_SCALE - 1);
    p->by1 = (int16_t)imin((oy + n * module) * R_SCALE - 1, R_H * R_SCALE - 1);
    s->qr = mods;
    s_extent[p - s->p] = -2.f;  // bbox preset
    return p;
}

static int text_width_q4(const font_t *f, const char *utf8, int track) {
    int w = 0, n = 0, prev = -1;
    while (*utf8) {
        int g = font_glyph(f, font_utf8_next(&utf8));
        w += f->g[g].adv + (prev >= 0 ? font_kern(f, prev, g) : 0);
        prev = g;
        n++;
    }
    return w + (n > 1 ? (n - 1) * track : 0);
}

float text_width(int font, const char *utf8, float spacing) {
    if (font < 0 || font >= FONT_COUNT || !utf8) return 0;
    return text_width_q4(&g_fonts[font], utf8, (int)lrintf(spacing * 16)) / 16.f;
}

size_t label_prefix(text_role_t role, const char *utf8, size_t max_bytes, float width, const char *suffix) {
    const text_style_t *style = &g_text[role];
    const font_t *font = &g_fonts[style->font];
    int track = (int)lrintf(style->track * 16);
    int tail_width = text_width_q4(font, suffix, track), tail_first = -1;
    if (*suffix) tail_first = font_glyph(font, font_utf8_next(&suffix));
    int advance = 0, prev = -1;
    const char *p = utf8;
    size_t fit = 0;
    while (*p) {
        int glyph = font_glyph(font, font_utf8_next(&p));
        size_t bytes = (size_t)(p - utf8);
        if (bytes > max_bytes) break;
        advance += font->g[glyph].adv + (prev < 0 ? 0 : track + font_kern(font, prev, glyph));
        int tail_join = tail_first < 0 ? 0 : track + font_kern(font, glyph, tail_first);
        if ((advance + tail_width + tail_join) / 16.f > width) break;
        fit = bytes; prev = glyph;
    }
    return fit;
}

static prim_t *make_text_primitive(scene_t *s, const font_t *f, int font, int start, int len, float x, float y,
                                   int align, float spacing, uint32_t rgb, float alpha) {
    int track = (int)lrintf(spacing * 16), w = (len - 1) * track;
    for (int i = 0; i < len; i++) w += f->g[s->text[start + i]].adv + (i ? font_kern(f, s->text[start + i - 1], s->text[start + i]) : 0);
    prim_t *p = &s->p[s->n++];
    memset(p, 0, sizeof(*p));
    p->kind = PK_TEXT;
    p->sub_r = track;
    p->alpha = alpha >= 1.f ? 255 : (uint8_t)(alpha * 255.f);
    p->color = rgb565(rgb);
    p->a = start;
    p->b = len;
    p->r = font;
    int ox = (int)lrintf(x - (align < 0 ? 0 : align == 0 ? w / 32.f : w / 16.f));
    int base = (int)lrintf(y);
    p->sub_x = ox;
    p->sub_y = base;
    p->clip = -1;
    p->bx0 = (int16_t)imax(ox - 2, 0);
    p->by0 = (int16_t)imax(base - f->ascent - 2, 0);
    p->bx1 = (int16_t)imin(ox + (w + 15) / 16 + 2, R_W * R_SCALE - 1);
    p->by1 = (int16_t)imin(base + f->descent + 2, R_H * R_SCALE - 1);
    s_extent[p - s->p] = -2.f;
    return p;
}

prim_t *sc_text(scene_t *s, int font, const char *utf8, float x, float y, int align, float spacing, uint32_t rgb,
                float alpha) {
    if (s->n >= R_MAX_PRIMS || alpha <= 0.004f || !utf8 || !*utf8 || font < 0 || font >= FONT_COUNT) return NULL;
    const font_t *f = &g_fonts[font];
    int start = s->text_n;
    for (const char *c = utf8; *c && s->text_n < R_TEXT_POOL;)
        s->text[s->text_n++] = (uint8_t)font_glyph(f, font_utf8_next(&c));
    int len = s->text_n - start;
    if (len <= 0) return NULL;
    return make_text_primitive(s, f, font, start, len, x, y, align, spacing, rgb, alpha);
}

prim_t *sc_label(scene_t *s, text_role_t role, const char *utf8, float x, float cy, int align, uint32_t rgb, float alpha) {
    const text_style_t *st = &g_text[role];
    return sc_text(s, st->font, utf8, x, cy + g_fonts[st->font].cap * .5f, align, st->track, rgb, alpha);
}

float label_width(text_role_t role, const char *utf8) { return text_width(g_text[role].font, utf8, g_text[role].track); }

prim_t *sc_icon(scene_t *s, int icon, float x, float y, float size, uint32_t rgb, float alpha) {
    if (s->n >= R_MAX_PRIMS || icon < 0 || icon >= ICON_COUNT || size <= 0.f || alpha <= 0.004f) return NULL;
    prim_t *p = &s->p[s->n++];
    memset(p, 0, sizeof(*p));
    p->kind = PK_ICON;
    p->alpha = alpha >= 1.f ? 255 : (uint8_t)(alpha * 255.f);
    p->color = rgb565(rgb);
    p->a = (int32_t)lrintf(size * 16.f);  // Q4 size in full-resolution panel pixels
    p->b = icon;
    p->sub_x = (int32_t)lrintf(x * 16.f);
    p->sub_y = (int32_t)lrintf(y * 16.f);
    p->clip = -1;
    int half = p->a / 2;
    int left = p->sub_x - half, top = p->sub_y - half;
    int right = p->sub_x + (p->a - half), bottom = p->sub_y + (p->a - half);
    int x0 = left >= 0 ? left / 16 : -((-left + 15) / 16);
    int y0 = top >= 0 ? top / 16 : -((-top + 15) / 16);
    int x1 = right >= 0 ? (right + 15) / 16 - 1 : -((-right) / 16) - 1;
    int y1 = bottom >= 0 ? (bottom + 15) / 16 - 1 : -((-bottom) / 16) - 1;
    p->bx0 = (int16_t)imax(x0, 0);
    p->by0 = (int16_t)imax(y0, 0);
    p->bx1 = (int16_t)imin(x1, R_W * R_SCALE - 1);
    p->by1 = (int16_t)imin(y1, R_H * R_SCALE - 1);
    s_extent[p - s->p] = -2.f;  // preset full-resolution overlay bounds
    return p;
}

prim_t *sc_image(scene_t *s, uint32_t id, int x, int y, int bx0, int by0, int bx1, int by1, float alpha) {
    if (s->n >= R_MAX_PRIMS || alpha <= 0.004f) return NULL;
    prim_t *p = &s->p[s->n++];
    memset(p, 0, sizeof(*p));
    p->kind = PK_SPRITE;
    p->alpha = alpha >= 1.f ? 255 : (uint8_t)(alpha * 255.f);
    p->a = (int32_t)id;
    p->cx = x;
    p->cy = y;
    p->clip = -1;
    p->bx0 = (int16_t)imax(bx0, 0);
    p->by0 = (int16_t)imax(by0, 0);
    p->bx1 = (int16_t)imin(bx1, R_W * R_SCALE - 1);
    p->by1 = (int16_t)imin(by1, R_H * R_SCALE - 1);
    s_extent[p - s->p] = -2.f;  // bbox preset
    return p;
}

prim_t *sc_circle(scene_t *s, float cx, float cy, float r, uint32_t rgb, float alpha) {
    return sc_rbox(s, cx, cy, r, r, r, 0.f, rgb, alpha);
}

prim_t *sc_capsule(scene_t *s, float x0, float y0, float x1, float y1, float thick, uint32_t rgb, float alpha) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    float r = thick * 0.5f;
    float c = 1.f, n = 0.f;  // the direction's cos and sin, without the angle
    if (len > 0.001f) { c = dx / len; n = dy / len; }
    return rbox_cs(s, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, len * 0.5f + r, r, r, c, n, rgb, alpha);
}

prim_t *sc_ring(scene_t *s, float cx, float cy, float radius, float thick, uint32_t rgb, float alpha) {
    XF_PREP(s, cx, cy, 0.f);
    radius = SZ(s, radius);
    thick = SZ(s, thick);
    prim_t *p = add(s, PK_RING, cx, cy, 0.f, rgb, alpha);
    if (!p) return NULL;
    p->a = (int32_t)(radius * Q);
    p->r = (int32_t)(thick * 0.5f * Q);
    set_extent(s, p, radius + thick);
    return p;
}

prim_t *sc_arc(scene_t *s, float cx, float cy, float radius, float half_ap, float thick, float angle, uint32_t rgb,
               float alpha) {
    if (half_ap < 0.01f) return NULL;
    XF_PREP(s, cx, cy, angle);
    radius = SZ(s, radius);
    thick = SZ(s, thick);
    prim_t *p = add(s, PK_ARC, cx, cy, angle, rgb, alpha);
    if (!p) return NULL;
    p->a = (int32_t)(radius * Q);
    p->r = (int32_t)(thick * 0.5f * Q);
    p->ap_s = (int16_t)lrintf(sinf(half_ap) * 16383.f);
    p->ap_c = (int16_t)lrintf(cosf(half_ap) * 16383.f);
    set_extent(s, p, radius + thick);
    return p;
}

prim_t *sc_heart(scene_t *s, float cx, float cy, float size, float angle, uint32_t rgb, float alpha) {
    XF_PREP(s, cx, cy, angle);
    size = SZ(s, size);
    if (size < 2.f) return NULL;
    prim_t *p = add(s, PK_HEART, cx, cy, angle, rgb, alpha);
    if (!p) return NULL;
    float K = size / 1.1f;
    p->a = (int32_t)(K * Q);
    set_extent(s, p, size * 0.75f);
    return p;
}

void pr_soft(prim_t *p, float px) {
    if (!p) return;
    px *= s_scale;
    if (px < 1.f) px = 1.f;
    p->soft = (int16_t)(px * Q);
}

void pr_stroke(prim_t *p, float width) {
    if (!p) return;
    width *= s_scale;
    if (width < 0.5f) width = 0.5f;
    p->stroke = (int32_t)(width * Q);
}

void pr_cut(prim_t *p, float nx, float ny, float c) {
    if (!p || p->ncut >= 2) return;
    int i = p->ncut++;
    // Local axes scale by (s_sx, s_sy): the plane normal scales inversely.
    float mx = nx / s_sx, my = ny / s_sy, l = sqrtf(mx * mx + my * my);
    if (l < 1e-6f) l = 1e-6f;
    p->cut_nx[i] = (int16_t)lrintf(mx / l * 16383.f);
    p->cut_ny[i] = (int16_t)lrintf(my / l * 16383.f);
    p->cut_c[i] = (int32_t)(c / l * Q);
}

void pr_sub_circle(prim_t *p, float lx, float ly, float r) {
    if (!p) return;
    p->has_sub = 1;
    p->sub_x = (int32_t)(lx * s_sx * Q);
    p->sub_y = (int32_t)(ly * s_sy * Q);
    p->sub_r = (int32_t)(r * s_scale * Q);
}

void pr_clip(scene_t *s, prim_t *p, const prim_t *clip) {
    if (!p || !clip) return;
    p->clip = (int8_t)(clip - s->p);
}

uint32_t rgb_mix(uint32_t a, uint32_t b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    int r = ar + (int)((br - ar) * t), g = ag + (int)((bg - ag) * t), bl = ab + (int)((bb - ab) * t);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

uint32_t rgb_scale(uint32_t a, float k) {
    if (k < 0) k = 0;
    int r = (int)(((a >> 16) & 255) * k), g = (int)(((a >> 8) & 255) * k), b = (int)((a & 255) * k);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
