#pragma once
#include <stdint.h>
#include <stdbool.h>

#define R_EMOJI_SLOTS 4
#define R_EMOJI_BYTES 576
#define R_EMOJI_EDGE 32
#define R_EMOJI_TOKEN 0x100
#define R_EMOJI_PLACEHOLDER (R_EMOJI_TOKEN + R_EMOJI_SLOTS)
typedef struct {
    char keys[R_EMOJI_SLOTS][65];
    uint8_t data[R_EMOJI_SLOTS][R_EMOJI_BYTES];
    uint8_t ready[R_EMOJI_SLOTS];
} render_emoji_snapshot_t;

// Normal glyphs retain the existing advance and kerning; emoji are a fixed box.
