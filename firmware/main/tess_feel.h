// How Tess's mood shows (tess_feel.c): the style each mood asks of the body, the blended style now, and the cue it plays on entering one.
#pragma once

#include "mood.h"

// What a mood changes, all as gains on what the neutral Tess does. Only turns and views: the camera's distance and tip, the
// distance the 4th axis is seen from, a turn in the x-w plane, the size of the body as a whole; and everything the dots do.
typedef struct {
    float zoom, cam, wdist, xw, pitch, lift;              // the body's size, camera distance (units), 4th-axis distance, x-w turn (rad), camera tip (rad), carried down (units)
    float dot, halo, tremble, breath, pulse;              // dot size, halo, random push on the dots (units/s), breath depth, heartbeat depth
    float spin, rest, turn4d, attention, saccade;         // turn speed, odds of resting, pace of 4D turns, how much it faces you, glance pace
    float flinch, dodge, invite;                          // how much a tap makes it flinch, how often it dodges, how often it asks for attention
} tess_style_t;
#define TESS_STYLE_FIELDS 19

typedef struct {
    uint32_t events;              // ME_BIT()s since the last update
    float event_strength[ME_COUNT], event_side[ME_COUNT]; // payload kept with each event through the batch
    float shown, palette;         // 0..1 how much of the mood the body / the colours show (the gate, eased)
    float level[MOOD_COUNT];      // how strongly each mood is shown now (kept while it fades out)
    float mix[MOOD_COUNT];        // the colour weights: blend x palette, in 1/32 steps (palette cache key)
    float huff_in, huff;          // grumpy: s to the next huff, and its envelope
    float cue_age;                // s since a cue was last queued (by anything, tess_cue)
    float cue_delay, cue_life, cue_strength, cue_side;  // an entry cue waiting: s until it may play, s until it is dropped
    uint8_t cue;                  // (tess_cue_t)
    bool cue_pending, was_online, lost;
    bool off;                     // (tests, the simulator) the machine does not run: a neutral Tess, as before there were moods
    uint32_t rng;                 // the huffs' own dice
} tess_feel_t;
