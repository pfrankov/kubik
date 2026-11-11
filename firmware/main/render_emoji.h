#pragma once
#include <stdint.h>
#include <stdbool.h>

#define R_EMOJI_SLOTS 4
#define R_EMOJI_BYTES 576
#define R_EMOJI_EDGE 32
#define R_EMOJI_TOKEN 0x100
#define R_EMOJI_PLACEHOLDER (R_EMOJI_TOKEN + R_EMOJI_SLOTS)
typedef struct render_emoji_entry render_emoji_entry_t;
// Bounded immutable payloads. One face reference, plus any in-flight scene pins.
render_emoji_entry_t *render_emoji_entry_create(const uint8_t pixels[R_EMOJI_BYTES]);
void render_emoji_entry_retain(render_emoji_entry_t *entry);
void render_emoji_entry_release(render_emoji_entry_t *entry);
const uint8_t *render_emoji_entry_pixels(const render_emoji_entry_t *entry);
uint32_t render_emoji_entry_hash(const render_emoji_entry_t *entry);
unsigned render_emoji_entries_live(void);
typedef struct {
    char keys[R_EMOJI_SLOTS][65];
    render_emoji_entry_t *entries[R_EMOJI_SLOTS];
    uint8_t ready[R_EMOJI_SLOTS];
} render_emoji_snapshot_t;
void render_emoji_snapshot_release(render_emoji_snapshot_t *snapshot);

// Normal glyphs retain the existing advance and kerning; emoji are a fixed box.
