// Completed, lossless RGB565 frame. Render first; only then present it to the panel.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "render.h"
#define FRAME_STRIPE 16  // native pixels: the unit a row is kept in (canvas.h's stripes)
#define FRAME_STRIPES (R_W / FRAME_STRIPE)
#ifndef FRAME_STORE_BYTES  // (the host test shrinks it)
#define FRAME_STORE_BYTES (48 * 1024)
#endif
typedef struct {
    uint32_t data[FRAME_STORE_BYTES / 4];  // packets of 32-bit words (see frame_store.c)
    uint32_t row[R_H];                     // where each row's packets start, in words
    uint16_t stripes[R_H];                 // which stripes of each row are kept (bit k: pixels 16k..16k+15)
    uint32_t used;                         // bytes used
    bool valid;
} frame_store_t;
void frame_store_begin(frame_store_t *f);
// Keeps the stripes of row y (pixels: the whole row, 4-byte aligned). What is not kept is not readable: a read fills
// the kept stripes only.
bool frame_store_row(frame_store_t *f, int y, uint32_t stripes, const uint16_t *pixels);
void frame_store_read(void *ctx, int y, uint16_t *out, int xa, int xb);  // fills the kept stripes of [xa, xb) (whole pixel pairs, at least)
