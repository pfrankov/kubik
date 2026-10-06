#include "render_internal.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#if RENDER_GLASS_ENABLED

#define Q 16
#define GSUB 4  // outline coverage samples per row

static int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

// ------------------------------------------------------------------ glass

#define GSUB 4  // outline coverage samples per row
static uint16_t s_glass_snap[R_W * R_BAND];      // the band as the video left it, before the face
static int16_t s_gx0, s_gx1, s_gy0, s_gy1;         // union box of the glass-only primitives
static int16_t s_gl[R_H * GSUB], s_gr[R_H * GSUB];  // outline span per sub-row, native px Q4 (empty: l > r)

static float s_glass_cos[24], s_glass_sin[24];
static int s_glass_trig_n;

static void glass_trig(int n) {
    if (s_glass_trig_n == n) return;
    for (int k = 0; k < n; k++) {
        float a = 6.2831853f * k / n;
        s_glass_cos[k] = cosf(a);
        s_glass_sin[k] = sinf(a);
    }
    s_glass_trig_n = n;
}

static bool glass_bounds(const scene_t *s) {
    s_gx0 = R_W;
    s_gy0 = R_H;
    s_gx1 = s_gy1 = -1;
    for (int i = s->glass_a; i < s->glass_b; i++) {
        const prim_t *p = &s->p[i];
        if (p->alpha == 0 || p->bx0 > p->bx1) continue;
        s_gx0 = (int16_t)imin(s_gx0, p->bx0);
        s_gx1 = (int16_t)imax(s_gx1, p->bx1);
        s_gy0 = (int16_t)imin(s_gy0, p->by0);
        s_gy1 = (int16_t)imax(s_gy1, p->by1);
    }
    return s_gy0 <= s_gy1;
}

static void glass_span_sample(int j, int16_t xi) {
    if (xi < s_gl[j]) s_gl[j] = xi;
    if (xi > s_gr[j]) s_gr[j] = xi;
}

static void glass_edge_integer(int j0, int j1, int32_t xq, int32_t dq) {
    for (int j = j0; j <= j1; j++, xq += dq) glass_span_sample(j, (int16_t)((xq + 32768) >> 16));
}

static void glass_edge_float(int j0, int j1, float x, float dx) {
    for (int j = j0; j <= j1; j++, x += dx) glass_span_sample(j, (int16_t)lrintf(x));
}

static void glass_scan_edge(int k, int n, const float *vx, const float *vy) {
    int k2 = (k + 1) % n;
    float ax = vx[k], ay = vy[k], bx = vx[k2], by = vy[k2];
    if (ay > by) { float t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; }
    if (by - ay < 1e-4f) return;
    int j0 = imax((int)ceilf(ay * GSUB - 0.5f), s_gy0 * GSUB);
    int j1 = imin((int)floorf(by * GSUB - 0.5f), (s_gy1 + 1) * GSUB - 1);
    if (j0 > j1) return;
    float dx = (bx - ax) / (by - ay), x = ax + ((j0 + 0.5f) / GSUB - ay) * dx;
    dx *= 16.f / GSUB;
    x *= 16.f;
    if (fabsf(dx) < 16384.f && fabsf(x) < 16384.f) {
        int32_t xq = (int32_t)lrintf(x * 65536.f), dq = (int32_t)lrintf(dx * 65536.f);
        glass_edge_integer(j0, j1, xq, dq);
        return;
    }
    glass_edge_float(j0, j1, x, dx);
}

static void glass_scan_outline(const scene_t *s) {
    glass_trig(s->glass_n);
    float vx[24], vy[24];
    for (int k = 0; k < s->glass_n; k++) {
        vx[k] = (s->glass_cx + s->glass_r[k] * s_glass_cos[k]) / R_SCALE;
        vy[k] = (s->glass_cy + s->glass_r[k] * s_glass_sin[k]) / R_SCALE;
    }
    for (int k = 0; k < s->glass_n; k++) glass_scan_edge(k, s->glass_n, vx, vy);
}

static void glass_box(const scene_t *s) {
    s_gx0 = s_gy0 = 1;
    s_gx1 = s_gy1 = 0;
    if (s->glass_a < 0 || s->glass_b <= s->glass_a || s->glass_n < 3) return;
    if (!glass_bounds(s)) return;
    for (int j = s_gy0 * GSUB; j < (s_gy1 + 1) * GSUB; j++) {
        s_gl[j] = INT16_MAX;
        s_gr[j] = INT16_MIN;
    }
    glass_scan_outline(s);
}

void render_glass_prepare(const scene_t *s) { glass_box(s); }

bool render_glass_intersects(int x0, int x1, int y0, int y1) {
    return s_gx0 <= s_gx1 && s_gy0 <= y1 && s_gy1 >= y0 && s_gx0 <= x1 && s_gx1 >= x0;
}

void render_glass_snapshot(uint16_t *buf, int width) {
    memcpy(s_glass_snap, buf, (size_t)width * R_BAND * sizeof(*buf));
}

// Puts the video back wherever the face drew off the glass outline.
static void glass_row_intervals(const int16_t *gl, const int16_t *gr, int *in0, int *in1, int *out0, int *out1) {
    *in0 = INT32_MIN / 2;
    *in1 = INT32_MAX / 2;
    *out0 = INT32_MAX / 2;
    *out1 = INT32_MIN / 2;
    for (int j = 0; j < GSUB; j++) {
        *in0 = imax(*in0, (gl[j] + 15) >> 4);
        *in1 = imin(*in1, (gr[j] >> 4) - 1);
        if (gl[j] <= gr[j]) {
            *out0 = imin(*out0, gl[j] >> 4);
            *out1 = imax(*out1, (gr[j] + 15) >> 4);
        }
    }
}

static inline int glass_coverage(int x, const int16_t *gl, const int16_t *gr) {
    int cov = 0;
    for (int j = 0; j < GSUB; j++) {
        int c = imin(gr[j], (x + 1) * 16) - imax(gl[j], x * 16);
        if (c > 0) cov += c;
    }
    return cov;
}

static R_HOT void glass_restore_pixel(uint16_t *row, const uint16_t *was, int x, const int16_t *gl, const int16_t *gr,
                                     int in0, int in1, int out0, int out1) {
    if (row[x] == was[x]) return;
    uint32_t keep = 32;
    if (x < in0 || x > in1) {
        if (x < out0 || x > out1) {
            row[x] = was[x];
            return;
        }
        keep = (uint32_t)glass_coverage(x, gl, gr) * 32 / (16 * GSUB);
    }
    if (keep >= 32) return;
    if (keep == 0) {
        row[x] = was[x];
        return;
    }
    uint16_t fg = g_render_swap ? rgb565_bswap(row[x]) : row[x];
    uint16_t o = rgb565_blend(fg, g_render_swap ? rgb565_bswap(was[x]) : was[x], keep);
    row[x] = g_render_swap ? rgb565_bswap(o) : o;
}

static R_HOT void glass_restore_row(uint16_t *row, const uint16_t *was, int xs, int xe, const int16_t *gl,
                                    const int16_t *gr) {
    int in0, in1, out0, out1;
    glass_row_intervals(gl, gr, &in0, &in1, &out0, &out1);
    for (int x = xs; x <= xe; x++) glass_restore_pixel(row, was, x, gl, gr, in0, in1, out0, out1);
}

static R_HOT void glass_restore(uint16_t *buf, int bx0, int bw, int y0, int y1) {
    int xs = imax(s_gx0, bx0), xe = imin(s_gx1, bx0 + bw - 1);
    int ya = imax(s_gy0, y0), yb = imin(s_gy1, y1);
    for (int y = ya; y <= yb; y++) {
        uint16_t *row = buf + (y - y0) * bw - bx0;
        const uint16_t *was = s_glass_snap + (y - y0) * bw - bx0;
        const int16_t *gl = s_gl + y * GSUB, *gr = s_gr + y * GSUB;
        glass_restore_row(row, was, xs, xe, gl, gr);
    }
}

void render_glass_restore(uint16_t *buf, int x0, int width, int y0, int y1) {
    glass_restore(buf, x0, width, y0, y1);
}

#endif
