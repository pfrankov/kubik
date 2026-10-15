#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t frames, elapsed_us, mean_us, max_us, late, px, prims, face, shapes, prep, image, glass, hash, send, gap_us, modes, presented, overflows;
    uint32_t slow_frames, deadline_misses;
    int active;
} frame_diagnostics_snapshot_t;
void frame_diagnostics_init(void);
bool frame_diagnostics_requested(void);
void frame_diagnostics_publish(const frame_diagnostics_snapshot_t *snapshot);
void frame_diagnostics_reply(char *reply, size_t capacity);
