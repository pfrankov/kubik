// Stage native panel output; layout and touch remain in design pixels.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DISPLAY_S3_WIDTH 368
#define DISPLAY_S3_HEIGHT 448
#define DISPLAY_S3_ROWS 8

// Called after the physical panel has been cleared. Reuses allocations on reset.
bool display_s3_init(void);
void display_s3_begin_frame(void);
// Sends the already prepared native damage after TE; returns its start time or 0.
int64_t display_s3_present(void);
void display_s3_begin(int x0, int y0, int x1, int y1);
// Returns the next window row for direct wire-order RGB565 composition.
// Rows are claimed in order; fill exactly x1-x0 pixels in each before end().
// Right-hand render padding is retained in storage and clipped when sent.
uint16_t *display_s3_next_row(void);
// Accumulates this completed window's send hulls; its pixels are already prepared.
void display_s3_end(void);
// Native points outside the physical panel are not UI contacts.
// Invalid points leave both outputs unchanged.
bool display_s3_touch(int native_x, int native_y, int16_t *logical_x, int16_t *logical_y);

// Measurements for the most recently presented frame, excluding scene rendering.
typedef struct {
    int64_t render_us, te_us, burst_us;
    // Composed native work includes repeated writes to overlapping windows.
    uint32_t input_pixels, native_pixels, windows;
} display_s3_profile_t;
display_s3_profile_t display_s3_last_profile(void);
