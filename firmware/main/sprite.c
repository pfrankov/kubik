#include "sprite.h"

#include <math.h>
#include <string.h>

#define G 240     // storage grid
#define HDR 16    // pack header size
#define ASZ 24    // anim entry size
#define FSZ 72    // frame entry size

typedef struct __attribute__((packed)) {
    uint32_t off, len;
    int16_t cx4, cy4, hw4, hh4, sin14;
    uint8_t vis, x0, x1, pad[3];
    int16_t quad4[8];  // TL, TR, BR, BL of the face design rectangle, panel px Q4
    int16_t gx4, gy4;  // glass outline centre, panel px Q4
    uint8_t gr[SPRITE_GLASS_N];  // ... radii, panel px / 2
    uint8_t pad2[4];
} frame_t;
_Static_assert(sizeof(frame_t) == FSZ, "frame entry");

static sprite_fetch_fn s_fetch;
static const uint8_t *s_head;  // header + palette + tables (kept mapped)
static uint16_t s_nanims;
static uint32_t s_nframes;
static const uint16_t *s_pal;
static const sprite_anim_t *s_anims;
static const uint8_t *s_frames_raw;
static const uint8_t *s_links;  // {frame, q} per frame per anim
static uint16_t s_pal_ram[256];

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void raster(const prim_t *p, uint16_t *buf, int bx0, int bw, int ya, int yb, int band_y0);

bool sprite_init(sprite_fetch_fn fetch) {
    s_fetch = fetch;
    s_head = NULL;
    const uint8_t *h = fetch(0, HDR, 0, HDR);
    if (!h || memcmp(h, "KSPR", 4) != 0 || rd16(h + 12) != G) return false;
    uint16_t version = rd16(h + 4);
    if (version != 5) return false;
    s_nanims = rd16(h + 6);
    s_nframes = rd32(h + 8);
    uint32_t links = 2u * s_nframes * s_nanims;
    uint32_t tables = HDR + 512 + ASZ * s_nanims + FSZ * s_nframes + links;
    h = fetch(0, tables, 0, tables);
    if (!h) return false;
    s_pal = (const uint16_t *)(h + HDR);
    s_anims = (const sprite_anim_t *)(h + HDR + 512);
    s_frames_raw = h + HDR + 512 + ASZ * s_nanims;
    s_links = s_frames_raw + FSZ * s_nframes;
    s_head = h;
    memcpy(s_pal_ram, s_pal, sizeof(s_pal_ram));
    render_set_image_fn(raster);
    return true;
}

void sprite_deinit(void) { s_head = NULL; s_fetch = NULL; render_set_image_fn(NULL); }

bool sprite_ready(void) { return s_head != NULL; }

int sprite_find(const char *name) {
    for (int i = 0; s_head && i < s_nanims; i++)
        if (strncmp(s_anims[i].name, name, sizeof(s_anims[i].name)) == 0) return i;
    return -1;
}

const sprite_anim_t *sprite_anim(int a) { return (s_head && a >= 0 && a < s_nanims) ? &s_anims[a] : NULL; }

int sprite_link(int a, int i, int to, int *q) {
    const sprite_anim_t *an = sprite_anim(a);
    if (!an || !sprite_anim(to) || i < 0 || i >= an->count) {
        *q = 255;
        return 0;
    }
    const uint8_t *l = s_links + 2u * ((uint32_t)(an->first + i) * s_nanims + (uint32_t)to);
    *q = l[1];
    return l[0];
}

static inline const frame_t *frame_at(uint32_t fi) { return (const frame_t *)(s_frames_raw + fi * FSZ); }

static const frame_t *frame_of(int a, int i) {
    const sprite_anim_t *an = sprite_anim(a);
    if (!an || an->count == 0) return NULL;
    if (i < 0) i = 0;
    if (i >= an->count) i = an->count - 1;
    return frame_at(an->first + i);
}

// Show one actual pose. Cross-dissolving moving silhouettes creates ghosts;
// continuity between clips is provided by their pose graph, not transparency.
static int frame_index(float pos) {
    return pos > 0 ? (int)(pos + 0.5f) : 0;
}

bool sprite_screen(int a, float pos, sprite_screen_t *o) {
    int i = frame_index(pos);
    const frame_t *f = frame_of(a, i);
    if (!f) return false;
    o->cx = f->cx4 / 16.f;
    o->cy = f->cy4 / 16.f;
    o->hw = f->hw4 / 16.f;
    o->hh = f->hh4 / 16.f;
    o->ang = asinf(f->sin14 / 16384.f);
    o->vis = f->vis / 255.f;
    o->has_quad = true;
    for (int k = 0; k < 8; k++)
        o->quad[k] = f->quad4[k] / 16.f;
    o->has_glass = true;
    o->gx = f->gx4 / 16.f;
    o->gy = f->gy4 / 16.f;
    for (int k = 0; k < SPRITE_GLASS_N; k++) o->gr[k] = 2.f * f->gr[k];
    return true;
}

static const uint8_t *blob_of(uint32_t fi) {
    if (!s_head || fi >= s_nframes) return NULL;
    const frame_t *f = frame_at(fi);
    // Map the whole animation so every frame reuses one flash mapping.
    int a = 0;
    while (a + 1 < s_nanims && fi >= s_anims[a + 1].first) a++;
    const frame_t *first = frame_at(s_anims[a].first);
    const frame_t *last = frame_at(s_anims[a].first + s_anims[a].count - 1);
    uint32_t ho = first->off, hl = last->off + last->len - ho;
    return s_fetch(f->off, f->len, ho, hl);
}

prim_t *sc_sprite(scene_t *s, int a, float pos, int x, int y, float alpha) {
    int i = frame_index(pos);
    const frame_t *f = frame_of(a, i);
    if (!f) return NULL;
    uint32_t fi = (uint32_t)(((const uint8_t *)f - s_frames_raw) / FSZ);
    const uint8_t *b = blob_of(fi);
    if (!b || b[1] <= b[0]) return NULL;
    x &= ~1; y &= ~1;  // snap both panel axes to the native sprite grid
    int x0 = f->x0, x1 = f->x1, y0 = b[0], y1 = b[1];
    prim_t *p = sc_image(s, fi, x, y, x + x0 * 2, y + y0 * 2, x + x1 * 2 + 1, y + y1 * 2 - 1, alpha);
    return p;
}

void sprite_breathe(prim_t *p, float scale) {
    if (!p || scale == 1.f) return;
    p->sub_y = (int32_t)lrintf(65536.f / scale);
    p->by0 = (int16_t)fmaxf(0, floorf(450 + (p->by0 - 450) * scale) - 2);
    p->by1 = (int16_t)fminf(479, ceilf(450 + (p->by1 - 450) * scale) + 2);
}

// ------------------------------------------------------------------ raster

// Source columns s0..s1 of row r into src (indexed by source column); `clear` zeroes the gaps between the spans first.
static R_HOT void decode_row(const uint8_t *blob, int r, int s0, int s1, uint16_t *src, bool clear) {
    if (clear) memset(&src[s0], 0, (size_t)(s1 - s0 + 1) * 2);
    int y0 = blob[0], y1 = blob[1];
    if (r >= y0 && r < y1) {
        const uint8_t *p = blob + 2 + 2 * (y1 - y0) + rd16(blob + 2 + 2 * (r - y0));
        int ns = *p++;
        for (int sp = 0; sp < ns; sp++) {
            int x = p[0], n = p[1];
            p += 2;
            int a = x > s0 ? x : s0, b = x + n - 1 < s1 ? x + n - 1 : s1;
            const uint8_t *q = p - x;
#pragma GCC unroll 4
            for (int j = a; j <= b; j++) src[j] = s_pal_ram[q[j]];
            p += n;
        }
    }
}

// Writes native colour v with alpha a (0..32) into a row stored in the output byte order.
static inline void put(uint16_t *row, int x, uint16_t v, uint32_t a) {
    if (!g_render_swap) {
        row[x] = a >= 32 ? v : rgb565_blend(v, row[x], a);
    } else {
        row[x] = rgb565_bswap(a >= 32 ? v : rgb565_blend(v, rgb565_bswap(row[x]), a));
    }
}

typedef struct {
    int x0, x1, source_x0, source_x1;
} raster_span_t;

typedef struct {
    const prim_t *prim;
    const uint8_t *blob;
    uint16_t *buffer;
    raster_span_t span;
    int buffer_x, buffer_width, buffer_y;
    uint32_t alpha;
} raster_context_t;

static inline R_HOT bool raster_span(const prim_t *p, int bx0, int bw, raster_span_t *span) {
    span->x0 = p->bx0 > bx0 ? p->bx0 : bx0;
    span->x1 = p->bx1 < bx0 + bw - 1 ? p->bx1 : bx0 + bw - 1;
    if (span->x0 > span->x1) return false;
    span->source_x0 = span->x0 - p->cx;
    span->source_x1 = span->x1 - p->cx;
    return span->source_x0 >= 0 && span->source_x1 < G;
}

static inline R_HOT void decode_raster_row(const raster_context_t *r, int local_y, uint16_t *line, uint16_t *other) {
    const prim_t *p = r->prim;
    int x0 = r->span.source_x0, x1 = r->span.source_x1;
    if (!p->sub_y) {
        decode_row(r->blob, local_y, x0, x1, line, true);
        return;
    }

    // Vertical subpixel resampling of one pose, anchored at the feet.
    int source = 225 * 256 + (int)(((int64_t)(local_y - 225) * p->sub_y) >> 8);
    int y_source = source >> 8;
    unsigned fraction = (source & 255) >> 3;
    decode_row(r->blob, y_source, x0, x1, line, true);
    if (fraction) {
        decode_row(r->blob, y_source + 1, x0, x1, other, true);
        for (int x = x0; x <= x1; x++) line[x] = rgb565_blend(other[x], line[x], fraction);
    }
}

static inline R_HOT void blend_raster_row(const raster_context_t *r, const uint16_t *line, uint16_t *row) {
    for (int x = r->span.x0; x <= r->span.x1; x++) {
        uint16_t color = line[x - r->prim->cx];
        if (color || g_render_img_opaque_ok) put(row, x - r->buffer_x, color, r->alpha);
    }
}

static inline R_HOT void raster_row(const raster_context_t *r, int y, uint16_t *line, uint16_t *other) {
    int local_y = y - r->prim->cy;
    if (local_y < 0 || local_y >= G) return;
    uint16_t *row = r->buffer + (y - r->buffer_y) * r->buffer_width;
    if (r->alpha >= 32 && g_render_img_opaque_ok && !g_render_swap && !r->prim->sub_y) {
        // Nothing has been drawn on the black background yet: the spans go straight into the band's row.
        decode_row(r->blob, local_y, r->span.source_x0, r->span.source_x1,
                   row + r->span.x0 - r->buffer_x - r->span.source_x0, false);
        return;
    }
    decode_raster_row(r, local_y, line, other);
    if (r->alpha >= 32 && g_render_img_opaque_ok && !g_render_swap) {
        memcpy(row + r->span.x0 - r->buffer_x, line + r->span.source_x0,
               (size_t)(r->span.x1 - r->span.x0 + 1) * sizeof(uint16_t));
    } else {
        blend_raster_row(r, line, row);
    }
}

static R_HOT void raster(const prim_t *p, uint16_t *buf, int bx0, int bw, int ya, int yb, int band_y0) {
    uint32_t fi = (uint32_t)p->a;
    if (fi >= s_nframes) return;
    const uint8_t *blob = blob_of(fi);
    if (!blob) return;
    raster_context_t r = {
        .prim = p, .blob = blob, .buffer = buf,
        .buffer_x = bx0, .buffer_width = bw, .buffer_y = band_y0,
        .alpha = ((uint32_t)p->alpha + 1) >> 3,
    };
    if (!raster_span(p, bx0, bw, &r.span)) return;
    // The renderer has converted panel coordinates to the native 240px grid.
    // Decode and blend each native row once; display upscaling happens after
    // the complete framebuffer is rendered, outside this image primitive.
    uint16_t line[G] __attribute__((aligned(4))), other[G] __attribute__((aligned(4)));
    for (int y = ya; y <= yb; y++) raster_row(&r, y, line, other);
}
