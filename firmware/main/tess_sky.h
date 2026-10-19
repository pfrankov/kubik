// Skies: Tess's game of light and harmony (Tess only). She unravels into a stream of her own dots; stars float in the
// dark, each with a note. A star fills while the stream passes through it and sings as it fills; when every star of
// the sky has been lit the sky resolves into a chord and the next one grows from it. Each finger makes a temporary
// invisible black hole; permanent holes and tilt bend the stream. Lit stars keep their light until the next sky.
// No score, no text, no required timing: the stream makes the music, never a timing challenge.
//   tess_sky.c          the rounds: invitation, play, resolution, leaving; touch; sound
//   tess_sky_stream.c   the stream: 112 particles on integers
//   tess_sky_compose.c  a sky from a seed: harmony, layout and a solution proven by the real stream
//   tess_sky_plan.c     what a sky holds; tess_sky_melody.c its ending melody
//   tess_sky_summon.c   how she asks: a warm body vertex with a quiet pad
//   tess_sky_touch.c    independent temporary black holes under the fingers
//   tess_sky_draw.c     the picture
// The state lives in a block taken when a sky starts and returned when it ends.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SKY_PARTICLES 112   // TESS_N: the particles are her dots
#define SKY_STARS 6
#define SKY_HOLES 2
#define SKY_TOUCHES 2
#define SKY_HUMS (SKY_HOLES + SKY_TOUCHES)
#define SKY_LESSONS 10      // saved progression, now increasing variety instead of unlocking tool types
#define SKY_Q3 8            // particle positions: 1/8 px
#define SKY_W 480
#define SKY_FULL .8f        // a star this full stays lit and sings for the rest of this sky
#define SKY_PROBE_N SKY_PARTICLES // prove with the full stream, including its actual source jitter
#define SKY_HOLD_S 1.6f     // all full this long

typedef enum { SKY_OFF, SKY_UNRAVEL, SKY_PLAY, SKY_RESOLVE, SKY_FADE } sky_phase_t;  // unravel: her dots fly out; fade: no one is playing
typedef enum { SKY_PUZZLE, SKY_POUR, SKY_AURORA } sky_kind_t;
enum { COL_TEAL, COL_GOLD, COL_ROSE, COL_COUNT };

// Position and velocity in 1/8 px; wait and age count fixed 60 Hz physics ticks, independent of drawing cadence.
typedef struct {
    int16_t x, y, vx, vy;
    uint16_t wait, age;
    int8_t rx, ry, rvx, rvy; // signed remainders preserve sub-Q3 motion and acceleration between ticks
    uint8_t color, spare;
} sky_particle_t;

typedef struct {
    float x, y;             // where it is now (drifting stars move about their anchor)
    float ax, ay, orbit, spin, phase;  // anchor, radius and speed of the drift (orbit 0: still)
    float fill, glow;       // 0..1: how full it is; the flash of becoming full, fading
    float hz;               // its note
    uint8_t degree;         // ...as a step of the scale (sky_degree_hz)
    uint8_t color;          // its visual and musical shade (every particle can light every star)
    uint8_t inside;         // particles in it this frame
    bool sang;              // this sky has already sounded its first full note
} sky_star_t;

typedef struct {
    float x, y, r;          // invisible centre and gravitational reach
    int8_t spin;            // -1 or +1 circulation
} sky_hole_t;
// shown is the 0..1 field strength during touch/release; the field has no visible geometry.
typedef struct { float x, y, shown; int8_t spin; bool down; uint8_t id; } sky_touch_t;

// Where the stream comes from, its direction (unit vector) and pace (px/s).
typedef struct { float x, y, dx, dy, speed; } sky_source_t;

// Everything that makes a sky: composed from a seed, traced for its solution, then played.
typedef struct {
    uint8_t kind, count_stars, count_holes, root;  // root: the chord's first scale degree
    float gravity[2];       // a verified solving tilt (screen axes); zero for the first, finger-led sky
    bool touch_solution;
    float t;                // seconds in the sky
    float remainder;        // time carried into the next fixed physics tick
    float feed_scale;       // 1 in both play and the full-stream proof
    uint32_t rng;
    sky_source_t source;
    sky_star_t star[SKY_STARS];
    sky_hole_t hole[SKY_HOLES];
} sky_world_t;

// A sky's ending melody (tess_sky_melody.c): its stars in a tour, with passing tones, home to the tonic.
#define SKY_MELODY_MAX 16
typedef struct {
    int8_t degree;    // scale step for sky_degree_hz, 0..14
    uint8_t units;    // length in eighth-note units, 1..8
    int8_t star;      // the star whose note this is, or -1 for a passing tone
    uint8_t accent;   // 0..255 loudness shape
} sky_note_t;
typedef struct {
    uint8_t n, total_units;
    uint16_t unit_ms;            // 140..200
    int8_t tour[SKY_STARS];      // the order the melody visits the stars (a constellation line is drawn in this order), rest -1
    sky_note_t note[SKY_MELODY_MAX];
    int8_t chord[3];             // the closing chord: tonic, its third and fifth as scale steps
} sky_melody_t;

// How a sky was played, for its melody: the stars in the order they first became full, and when (s into the sky).
typedef struct { uint8_t n; int8_t order[SKY_STARS]; float at[SKY_STARS]; } sky_played_t;

// What a sky is made from, and the composer that makes it a slice at a time (tess_sky_compose.c).
typedef struct { uint8_t elements, skill, root; bool first, aurora; float from[2]; } sky_recipe_t;
typedef struct { uint8_t stars, holes, drift; bool pour; } sky_plan_t;
typedef struct {
    sky_world_t *w; uint32_t seed; sky_recipe_t r; sky_plan_t pl;
    uint8_t attempt, stage, tries; bool simple;
    uint16_t steps;
    sky_world_t probe;                  // a copy that is made to flow, to prove the layout before it is shown
    sky_particle_t light[SKY_PROBE_N];
} sky_composer_t;

typedef struct tess_sky {
    uint8_t phase;
    uint8_t solved_here;    // skies solved this session, saturates at 255
    sky_world_t w, next;    // the sky being played, and the next one growing while it resolves
    sky_composer_t comp;
    uint8_t compose;        // the next sky: 0 not begun, 1 growing, 2 grown
    sky_particle_t p[SKY_PARTICLES];
    sky_touch_t touch[SKY_TOUCHES];
    bool finger_down;
    float hold, struggle, resolve_t, resolve_len, fade;  // seconds: all full this long; in this sky; resolving (of how long); fading out
    uint8_t hints;          // hints shown in this sky
    int8_t hint_star;       // a still-dark target that softly asks for light; -1 none
    sky_played_t played;    // the order the player lit the stars in, for the melody
    sky_melody_t melody;    // the resolution's own melody, sung back from how it was played
    uint8_t melody_next, tour_lit;  // its next note; stars of its tour reached
    float sparkle_wait;     // the inflow's notes are rate limited
    float beat_wait;        // the shared pulse, gated by actual particle flow
    uint8_t beat_step, beat_flow[SKY_STARS]; // sparse rhythm and peak crossings since the last beat
    float pad_gain[SKY_STARS], hum_gain[SKY_HUMS], pad_sent[SKY_STARS], hum_sent[SKY_HUMS];  // what the voices were last told, and when ago
    uint32_t seed;
} tess_sky_t;
