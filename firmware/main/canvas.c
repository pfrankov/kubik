#include "canvas.h"
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define CANVAS_HOT IRAM_ATTR
#else
#define CANVAS_HOT
#endif

#define HASH_MUL 0x9E3779B1u
#define ZERO_STRIPE_HASH 0xF0229BE3u

_Static_assert(CANVAS_STRIPE == 16, "stripe_hash reads a stripe as eight words");

typedef struct { uint32_t hash, lit, mask; int changed0, changed1; } canvas_scan_t;
typedef struct { int first, last; } canvas_extent_t;

void canvas_reset(canvas_t *c) {
    if (c) c->previous_valid = false;
}

static inline uint32_t CANVAS_HOT stripe_hash(const uint16_t *p, bool aligned, uint32_t *any) {
    uint32_t h = 0x811C9DC5u, acc = 0;
    if (aligned) {
        const uint32_t *w = (const uint32_t *)(const void *)p;
        acc = w[0] | w[1] | w[2] | w[3];  // a lit stripe shows in its first words: the rest are read only for a dark one
        if (!acc) acc = w[4] | w[5] | w[6] | w[7];
        if (!acc) { *any = 0; return ZERO_STRIPE_HASH; }
#pragma GCC unroll 8
        for (int i = 0; i < CANVAS_STRIPE / 2; i++) { h = (h ^ w[i]) * HASH_MUL; h ^= h >> 15; }
    } else {
        for (int i = 0; i < CANVAS_STRIPE; i += 2) acc |= p[i] | (uint32_t)p[i + 1] << 16;
        if (!acc) { *any = 0; return ZERO_STRIPE_HASH; }
        for (int i = 0; i < CANVAS_STRIPE; i += 2) {
            uint32_t v = p[i] | (uint32_t)p[i + 1] << 16;
            h = (h ^ v) * HASH_MUL; h ^= h >> 15;
        }
    }
    *any = acc;
    return h;
}

static inline canvas_scan_t CANVAS_HOT scan_row(canvas_t *c, int y, const uint16_t *src, bool aligned) {
    canvas_scan_t scan = {.hash = 0x811C9DC5u, .lit = 0, .mask = 0, .changed0 = CANVAS_W, .changed1 = 0};
    uint32_t *stripes = c->stripe_hash[y];
    for (int k = 0; k < CANVAS_STRIPES; k++) {
        uint32_t any, hash = stripe_hash(src + k * CANVAS_STRIPE, aligned, &any);
        if (any) scan.lit |= 1u << k;
        if (hash != stripes[k]) {
            if (scan.changed0 == CANVAS_W) scan.changed0 = k * CANVAS_STRIPE;
            scan.changed1 = (k + 1) * CANVAS_STRIPE;
            stripes[k] = hash;
            scan.mask |= 1u << k;
        }
    }
    // Unchanged stripe hashes imply the identical ordered row hash, including collisions.
    if (c->previous_valid && !scan.mask) scan.hash = c->row_hash[y];
    else for (int k = 0; k < CANVAS_STRIPES; k++) scan.hash = (scan.hash ^ stripes[k]) * HASH_MUL;
    return scan;
}

static CANVAS_HOT canvas_extent_t row_extent(const uint16_t *src, uint32_t lit) {
    canvas_extent_t extent = {.first = CANVAS_W, .last = 0};
    if (lit) {
        int first = __builtin_ctz(lit), last = 31 - __builtin_clz(lit);
        for (extent.first = first * CANVAS_STRIPE; !src[extent.first]; extent.first++) {}
        for (extent.last = (last + 1) * CANVAS_STRIPE; !src[extent.last - 1]; extent.last--) {}
    }
    return extent;
}

static CANVAS_HOT void save_row(canvas_t *c, int y, canvas_scan_t scan, canvas_extent_t extent) {
    c->changed_x0[y] = c->changed_x1[y] = 0;
    c->changed_mask[y] = 0;
    if (!c->previous_valid) {
        c->changed_x1[y] = CANVAS_W;
        c->changed_mask[y] = (1u << CANVAS_STRIPES) - 1;
    } else if (scan.hash != c->row_hash[y]) {
        c->changed_mask[y] = (uint16_t)scan.mask;
        // Include old and new pixels, narrowed to changed stripes when available.
        int left = extent.first < c->row_start[y] ? extent.first : c->row_start[y];
        int right = extent.last > c->row_end[y] ? extent.last : c->row_end[y];
        if (scan.changed0 < scan.changed1) {
            if (scan.changed0 > left) left = scan.changed0;
            if (scan.changed1 < right) right = scan.changed1;
        }
        if (left < right) {
            c->changed_x0[y] = (uint8_t)left;
            c->changed_x1[y] = (uint8_t)right;
        }
    }
    c->row_hash[y] = scan.hash;
    c->row_start[y] = (uint8_t)extent.first;
    c->row_end[y] = (uint8_t)extent.last;
}

bool CANVAS_HOT canvas_rows(canvas_t *c, int y0, int y1, const uint16_t *src) {
    if (!c || !src || y0 < 0 || y1 <= y0 || y1 > CANVAS_H) return false;
    bool aligned = !((uintptr_t)src & 3);  // rows are 480 bytes: every row shares the alignment
    for (int y = y0; y < y1; y++, src += CANVAS_W) {
        canvas_scan_t scan = scan_row(c, y, src, aligned);
        canvas_extent_t extent = row_extent(src, scan.lit);
        save_row(c, y, scan, extent);
    }
    if (y1 == CANVAS_H) c->previous_valid = true;
    return true;
}
