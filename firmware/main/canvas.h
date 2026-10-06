// Which columns of each native row changed since the previous frame. Only hashes are kept, never the
// pixels.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CANVAS_W 240
#define CANVAS_H 240

typedef struct { int x0, y0, x1, y1; } canvas_rect_t; // exclusive end

// A changed row is sent only across the stripes that changed, not its whole lit extent: the listening
// border lights every row edge to edge.
#define CANVAS_STRIPE 16
#define CANVAS_STRIPES (CANVAS_W / CANVAS_STRIPE)

typedef struct {
    uint32_t row_hash[CANVAS_H];
    uint32_t stripe_hash[CANVAS_H][CANVAS_STRIPES];
    uint8_t row_start[CANVAS_H], row_end[CANVAS_H];      // lit (nonblack) extent
    uint8_t changed_x0[CANVAS_H], changed_x1[CANVAS_H];  // this frame's changed columns (empty: none)
    uint16_t changed_mask[CANVAS_H];                     // this frame's changed stripes, a bit each (a hull would join far-apart specks)
    bool previous_valid;
} canvas_t;

// Zero-initialized: the first frame counts as changed everywhere. canvas_reset() forgets the previous
// frame the same way (the panel no longer shows it).
void canvas_reset(canvas_t *c);
// Full native rows y0..y1-1 of a frame, in order; src holds (y1-y0)*240 pixels (4-byte aligned is
// fastest). Sets changed_x0/x1 for those rows; the frame becomes the previous one after row 239.
bool canvas_rows(canvas_t *c, int y0, int y1, const uint16_t *src);
