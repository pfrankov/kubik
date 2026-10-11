// Tess's play: her own structure is the toy. Taps pluck waves through her 4D edges, swipes spin her open,
// a pull beside her stretches and slings her, a hard landing shatters her and a sweep gathers her again;
// alone, she plays with herself. The app restores durable progress before the first frame; render-only
// fixtures remain a fully formed Tess until they opt into this model.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "tess_progress.h"

// Rounds that own the finger while they last. Plucks, spin and self-play are not rounds: they never take input.
typedef enum { TESS_GAME_NONE, TESS_GAME_STRETCH, TESS_GAME_FLY, TESS_GAME_GATHER } tess_game_t;
typedef enum { TESS_GAME_REST, TESS_GAME_SHOW, TESS_GAME_WAIT, TESS_GAME_CELEBRATE } tess_game_phase_t;
// Developer previews (Screen Lab), one per behaviour.
typedef enum { TESS_PLAY_PLUCK, TESS_PLAY_BLOOM, TESS_PLAY_STRETCH, TESS_PLAY_FLY, TESS_PLAY_GATHER, TESS_PLAY_SELF, TESS_PLAY_COUNT } tess_preview_t;
// What she does by herself when left alone.
typedef enum { SELF_NONE, SELF_PLUCK, SELF_REACH, SELF_SPIN, SELF_WRESTLE } tess_self_t;

#define TESS_WAVES 4
#define TESS_GATHER_SLOTS 112  // the burst slots; face.h: TESS_N

// A wave running through her edges from one vertex, shell by shell of the hypercube. No two ring alike:
// each has its own pace (s per shell), ring rate and register. A negative amplitude is a recoil, not a ring.
typedef struct { uint8_t origin, quiet, rung; float age, amp, shell_s, ring_w, pitch; } tess_wave_t;

typedef struct {
    bool ready, available;
    uint32_t round;              // keeps deferred input out of a replacement round
    uint8_t progress, game, phase, tier;
    uint16_t streak;             // Gather: dots home
    float age, clock, idle;
    bool finger_down, finger_owned, pressed;  // owned: this finger belongs to a round
    float finger_age, finger_x, finger_y, finger_moved, lifted, sweep[2];  // sweep: Gather's last point
    tess_wave_t wave[TESS_WAVES];
    uint8_t grab;                // the vertex a stretch holds
    float pull[2], pull_k;       // where it is pulled (panel px) and how hard, 0..1
    float fly[2], fly_v[2];      // the slung body: offset and velocity, units and units/s
    float squash[2], squash_k;   // the wall she last hit (its normal) and how flattened she still is
    float bloom, spin;           // how far spin has opened her, 0..1; her spin, rad/s
    uint8_t self_act, self_vertex, self_next;  // self_next: her repertoire in turn, so nothing repeats
    float self_age, self_wait, self_dir[2];
    float gather_clock, gather_start;
    uint16_t gathered_at[TESS_GATHER_SLOTS];  // Gather: 1 + centiseconds of gather_clock when swept home; 0 out
    // Her temper, built by what has been happening and fading on its own (each 0..1): excitement from play,
    // annoyance from being poked too fast, fear from hard knocks, fondness from gentle play.
    float excite, annoy, fear, fond;
    uint8_t last_corner;
    float since_pluck, since_touch, touch_at[2];  // what she remembers of the last touch
    uint8_t answer_corner;       // a pluck she will give back by herself
    float answer_in;             // s until it, < 0 none
    float tilt[2], tilt_rest[2]; // the device's tilt from how it usually stands (pull on the panel), and that usual
    bool tilt_known;             // the usual pose starts from the first reading, not from level
    float creak_in, whoosh_turn; // when the next stretch creak and spin whoosh sound
    uint16_t tone_kinds;         // one bit per kind of body sound she has made (for the USB probe)
    float tick_at;               // Gather: when its last dot sounded
    bool shaping;                // any structural play is moving her dots this frame
    float vertex_px[16][2];      // where her corners were last drawn, panel px (no projection on input paths)
    float form, fold, pulse, offset[2];
    float hit[2];                // her centre on the panel
    bool hit_valid;
    uint8_t trail_n;             // the point's fading positions, newest first, sampled at 18 Hz
    float trail[6][2], trail_at;
} tess_games_t;
