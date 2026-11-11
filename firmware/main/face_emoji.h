#pragma once
#include <stddef.h>
#include "render_emoji.h"

typedef struct face face_t;
typedef struct {
    const char *text;
    size_t bytes;
} face_emoji_key_t;
typedef struct {
    render_emoji_snapshot_t glyphs;
    uint32_t id;
    unsigned count;
    int page;
    float elapsed;
    bool valid, requested;
} face_emoji_t;
typedef struct {
    uint32_t id;
    unsigned count, ready, missing, page, pages;
} face_emoji_stats_t;

// Bounded, state-only operations; the caller owns the face lock. NULL is a
// terminal unavailable glyph. No requests retry without a page change/reset.
bool face_emoji_poll(face_t *f, uint32_t *id, char keys[4][65], unsigned *count);
bool face_emoji_receive(face_t *f, uint32_t id, const char *seq, const uint8_t *bytes);
void face_emoji_reset(face_t *f);
void face_emoji_stats(const face_t *f, face_emoji_stats_t *stats);
void face_emoji_sync(face_t *f);
void face_emoji_update(face_t *f, float dt);
// Add complete emoji keys by reference, including unsupported/oversized keys.
// false means adding this text would exceed the four-unique-key page limit.
bool face_emoji_collect(const char *text, size_t bytes, face_emoji_key_t keys[4], unsigned *count);
