// The frame output: the scene is composed completely first (the rows kept in a lossless store, what changed
// found by the canvas' hashes), and only then sent to the panel as windows, all in one burst, so a change of the
// whole screen is not painted while the next band is still being composed. The panel has no wired TE signal:
// the burst is as short as the panel bus allows. Host-testable: the panel and the clock are board.h / esp_timer.h.
#pragma once

#include <stdint.h>
#include "board.h"
#include "canvas.h"
#include "frame_store.h"
#include "render.h"

#define OUTPUT_ROWS 12  // fewer DMA submissions; three buffers use 34.5 KiB total
#define PLAN_WINDOWS (RENDER_BAND_WINDOWS + 4)  // the windows of a band: its changed stripes, the edge band's ring

typedef struct {
    canvas_t canvas;
    frame_store_t store;    // the rows of the completed frame the burst reads, packed: the panel rows are expanded from it
    int sent;               // the bands already sent (a frame too dense for the store goes out in two)
    unsigned flushes;
    int bands_kept;         // the bands of this frame decided for the store so far (all of them before the burst)
    const scene_t *scene;
    uint16_t *bands[2];     // render bands (native rows), alternating
    uint16_t *bufs[LCD_PIPE];  // panel rows for DMA
    int next;
    render_output_t output;
    uint16_t mask[CANVAS_H];  // the changed stripes of each native row (canvas.h): what the windows are cut from
    render_win_t plan[R_BANDS][PLAN_WINDOWS];  // the windows of each band, planned as it is kept
    uint8_t plan_n[R_BANDS];
    uint16_t plan_skip[R_BANDS];  // bit i: window i went into one of the band above
    render_win_t *open[PLAN_WINDOWS];  // the windows reaching the bottom of the band planned last
    int open_n;
    int16_t plan_reach[R_BANDS];  // the edge band's reach there (edge_reach), -1: unchanged
    uint16_t border_need;  // the stripes of the next band's first row the last planned band reads
    // Text/icons are composited after expansion. Diff their individual bounds,
    // so animating one icon does not resend the rectangle of every screen label.
    render_overlay_item_t overlay_prev[R_MAX_PRIMS];
    unsigned overlay_count;
    uint16_t overlay_mask[CANVAS_H];
    uint32_t edge_sig;  // the edge band's look this frame (render_edge_signature)
    // What the panel shows of the edge band, per band of rows (the larger of it and the new one covers both).
    uint8_t band_edge_w[R_BANDS], band_edge_r[R_BANDS];
    uint32_t band_edge_sig[R_BANDS];
    int heal_y, heal_h;  // heal_h 0: skipped (a busy frame); a band resent every frame whether changed or not: a mis-written pixel lasts <= 4 s
    int64_t heal_px0;
    int64_t burst_start;  // esp_timer time of the frame's first panel write (0: none yet)
    int64_t hash_us, pack_us, expand_us, px, windows;  // profiling
    int64_t burst_sum, burst_max, bursts;  // first to last panel write of a frame, profiling
    int64_t shown_at, gap_min, gap_max;  // the last frame's first panel write; the spacing of consecutive frames' (0: none)
    unsigned store_max, overflows;  // the largest packed frame; frames that did not fit (repainted whole next frame)
} frame_out_t;

// One frame: composes the scene, then sends what changed. No wait for the panel (its DMA tail runs on).
bool present_frame(frame_out_t *f, render_state_t *state, scene_t *scene);
