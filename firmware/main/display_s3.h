// Map the rectangular logical UI to every pixel of the S3 1.8-inch panel.
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
// Pixels are host-order RGB565, in complete rows of the logical window.
// Each copied chunk immediately prepares its native damage while cache-hot.
// Conversion to panel byte order happens once, after the final interpolation.
void display_s3_write(const uint16_t *pixels, int count, bool last);
// Accumulates this completed window's send hulls; its pixels are already prepared.
void display_s3_end(void);
// Native points outside the physical panel are not UI contacts.
// Invalid points leave both outputs unchanged.
bool display_s3_touch(int native_x, int native_y, int16_t *logical_x, int16_t *logical_y);

// Measurements for the most recently presented frame, excluding scene rendering.
typedef struct {
    int64_t copy_us, prepare_us, te_us, burst_us;
    // Resampling work: chunk halos and overlapping windows may prepare a pixel again.
    uint32_t input_pixels, native_pixels, windows;
} display_s3_profile_t;
display_s3_profile_t display_s3_last_profile(void);
