// Character body animation: frames from the sprite pack (tools/sprites/build.py).
// Portable C (also compiled by the host simulator).
//
// Frames are stored at half resolution (240x240 grid) as 8-bit palette indices
// in row spans. Rasterization stays on that grid; the completed framebuffer is
// upscaled by the display path. Index 0 is
// transparent (the panel background is black anyway).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "render.h"

// Returns a pointer to `len` bytes at `off` of the sprite pack. `hint_*` is a
// larger range worth mapping at once (the whole animation). The pointer must
// stay valid until the next call with a different hint range.
typedef const uint8_t *(*sprite_fetch_fn)(uint32_t off, uint32_t len, uint32_t hint_off, uint32_t hint_len);

typedef struct {
    char name[12];
    uint16_t first, count, loop_a, loop_b;
    uint8_t fps, flags;
    uint16_t join;  // first frame after the cut between two video segments (0 = none)
} sprite_anim_t;

#define SPRITE_GLASS_N 24

typedef struct {
    float cx, cy, hw, hh, ang;  // face screen in panel pixels (sprite origin at 0,0)
    float vis;                  // 0..1, how much of the screen is visible
    bool has_quad;              // perspective track present (pack v3)
    float quad[8];              // TL, TR, BR, BL of the face design rectangle, panel px
    bool has_glass;             // glass outline present (pack v4)
    float gx, gy;               // ... its centre, panel px
    float gr[SPRITE_GLASS_N];   // ... radii at 360/N degree steps from +x (clockwise on screen); 0 = gone
} sprite_screen_t;

bool sprite_init(sprite_fetch_fn fetch);
void sprite_deinit(void);
bool sprite_ready(void);
int sprite_find(const char *name);  // animation index or -1
const sprite_anim_t *sprite_anim(int a);
bool sprite_screen(int a, float pos, sprite_screen_t *out);
// Pose links (pack v5): the frame of anim `to` that best continues frame `i` of anim `a`,
// and how well: *q <= SPRITE_SEAMLESS is an invisible cut, 255 = far off (or no table).
#define SPRITE_SEAMLESS 100
int sprite_link(int a, int i, int to, int *q);
// Fractional positions select the closest actual pose, clamped at the last frame.
// Adds the frame as an image primitive with its origin at (x, y).
void sprite_breathe(prim_t *p, float scale);
prim_t *sc_sprite(scene_t *s, int a, float pos, int x, int y, float alpha);
