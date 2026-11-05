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
// Submits prepared damage after fresh TE; returns the actual first-pixel time
// before the DMA tail completes, or0 when there is no visible damage.
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

// Measurements for the most recently completed SPI frame, excluding scene rendering.
typedef struct {
    int64_t render_us, expand_us, edge_us, text_us, te_us, burst_us;
    uint64_t render_cycles;
    // Composed native work includes repeated writes to overlapping windows.
    uint32_t input_pixels, native_pixels, windows;
    int sender_core;
    uint32_t sender_stack_free;
} display_s3_profile_t;
display_s3_profile_t display_s3_last_profile(void);

// Attribute native composition work separately from staging and TE wait.
void display_s3_account_render(int64_t expand, int64_t edge, int64_t text);
