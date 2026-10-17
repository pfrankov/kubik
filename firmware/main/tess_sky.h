// Skies: Tess's game of light and harmony (Tess only). She unravels into a stream of her own dots; stars float in the
// dark, each with a note. A star fills while the stream passes through it and sings as it fills; when every star of
// the sky is full at once the sky resolves into a chord and the next one grows from it. The finger bends the stream,
// tilting the cube pours it, and strings, wells and prisms are moved with a finger to carry it where it has to go.
// No score, no text, no beat: nothing is ever wrong, and nothing waits for the player.
//   tess_sky.c          the rounds: invitation, play, resolution, leaving; touch; sound
//   tess_sky_stream.c   the stream: 112 particles on integers
//   tess_sky_compose.c  a sky from a seed: harmony, layout, a solution proven by tracing, the scramble
//   tess_sky_plan.c     what a sky holds; tess_sky_melody.c its ending melody
//   tess_sky_summon.c   how she asks: a wandering star with its own motif
//   tess_sky_touch.c    the finger: tools, relays, the aimed source
//   tess_sky_draw.c     the picture
// The state lives in a block taken when a sky starts and returned when it ends.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SKY_PARTICLES 112   // TESS_N: the particles are her dots
#define SKY_STARS 6
#define SKY_TOOLS 6
#define SKY_Q3 8            // particle positions: 1/8 px
#define SKY_W 480
#define SKY_FULL .8f        // a star this full sings; all of them at once resolve the sky
#define SKY_PROBE_N 28      // particles of a proof run (each feeds the stars four times as much)
#define SKY_HOLD_S 1.6f     // all full this long

typedef enum { SKY_OFF, SKY_UNRAVEL, SKY_PLAY, SKY_RESOLVE, SKY_FADE } sky_phase_t;  // unravel: her dots fly out; fade: no one is playing
typedef enum { SKY_PUZZLE, SKY_POUR, SKY_HARP, SKY_AURORA } sky_kind_t;
typedef enum { TOOL_NONE, TOOL_STRING, TOOL_WELL, TOOL_PRISM, TOOL_SPLIT, TOOL_HUNGRY } sky_tool_kind_t;
enum { COL_TEAL, COL_GOLD, COL_ROSE, COL_COUNT };
// What a sky may contain, met one per solved sky: the newest one is always in the next sky.
// A relay is a star that, once full, sends the light on its own way (a tap turns it); an aimed source turns with a finger.
typedef enum { EL_STAR, EL_POUR, EL_STRING, EL_RELAY, EL_WELL, EL_AIM, EL_PRISM, EL_SPLIT, EL_HUNGRY, EL_DRIFT, EL_COUNT } sky_element_t;
#define SKY_DIRS 8          // a relay sends the light one of eight ways

// 12 bytes: position and velocity in 1/8 px (and per second), the colour it carries, frames until it is let out, age.
typedef struct { int16_t x, y, vx, vy; uint8_t color, wait, age, spare; } sky_particle_t;

typedef struct {
    float x, y;             // where it is now (drifting stars move about their anchor)
    float ax, ay, orbit, spin, phase;  // anchor, radius and speed of the drift (orbit 0: still)
    float fill, glow;       // 0..1: how full it is; the flash of becoming full, fading
    float hz;               // its note
    uint8_t degree;         // ...as a step of the scale (sky_degree_hz)
    uint8_t color;          // the colour it takes (COL_*)
    uint8_t inside;         // particles in it this frame
    bool sang;              // it was full last frame
    bool relay;             // full, it turns what reaches its heart to `dir`
    uint8_t dir, sdir;      // a relay's way now and in the solution (0 = right, then clockwise by 45 deg)
} sky_star_t;

typedef struct {
    uint8_t kind, color, movable;
    int8_t held;            // a well: particles circling it, for its hum
    float x, y;             // the centre
    float dx, dy;           // a string or splitter: the half vector from the centre to one end
    float r;                // a well's or hungry star's reach, a prism's size
    float sx, sy, sdx, sdy; // where the solution has it
    float pluck_wait;       // a string's note rate limit
    float shown;            // 0..1 how far it has appeared
    bool snapped;           // put where the solution has it (it clicks there, and stays until pulled away)
} sky_tool_t;

// Where the stream comes from, its way (unit) and pace (px/s); an aimed one turns with a finger (s: its way in the solution).
typedef struct { float x, y, dx, dy, speed, sdx, sdy; bool aim; } sky_source_t;

// Everything that makes a sky: composed from a seed, traced for its solution, then played.
typedef struct {
    uint8_t kind, count_stars, count_tools, root;  // root: the chord's first scale degree
    float gravity[2];       // the tilt that solves a pour sky (screen axes); 0 when tools do it
    float t;                // seconds in the sky
    float feed_scale;       // 1; a sky traced with fewer particles feeds its stars proportionally more
    uint32_t rng;
    sky_source_t source;
    sky_star_t star[SKY_STARS];
    sky_tool_t tool[SKY_TOOLS];
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
typedef struct { uint8_t elements, newest, skill, root; bool first, aurora; float from[2]; } sky_recipe_t;
typedef struct { uint8_t stars, seq[4], seq_n, hungry, drift; bool pour, split, relay, aim; } sky_plan_t;
typedef struct {
    sky_world_t *w; uint32_t seed; sky_recipe_t r; sky_plan_t pl;
    uint8_t attempt, stage, j, tries; bool simple;
    uint16_t steps;
    float last;
    sky_world_t probe;                  // a copy that is made to flow, to prove the layout before it is scrambled
    sky_particle_t light[SKY_PROBE_N];
} sky_composer_t;

typedef struct tess_sky {
    uint8_t phase;
    uint8_t solved_here;    // skies solved this visit
    sky_world_t w, next;    // the sky being played, and the next one growing while it resolves
    sky_composer_t comp;
    uint8_t compose;        // the next sky: 0 not begun, 1 growing, 2 grown
    sky_particle_t p[SKY_PARTICLES];
    int8_t grab, grab_end;  // the tool a finger holds (and which end of a string, -1 the middle), -1 none
    float grab_dx, grab_dy;
    bool finger_down, aiming;   // aiming: the finger turns the source
    int8_t tap_star;        // a relay under a finger that came down (a quick tap turns it), -1 none
    float press_t, press[2];    // how long the finger has been down, and where it came down
    float finger[2];
    float hold, calm, struggle, resolve_t, resolve_len, fade;  // seconds: all full this long; without touch or tilt; in this sky; resolving (of how long); fading out
    uint8_t hints;          // hints shown in this sky
    int8_t hint_tool;       // what quivers: a tool, SKY_TOOLS + a relay star, SKY_TOOLS + SKY_STARS the source; -1 none
    sky_played_t played;    // the order the player lit the stars in, for the melody
    sky_melody_t melody;    // the resolution's own melody, sung back from how it was played
    uint8_t melody_next, tour_lit;  // its next note; stars of its tour reached
    uint8_t snaps, turns;   // sound events of touch since the last frame: tools that clicked into place, relays turned
    float sparkle_wait;     // the inflow's notes are rate limited
    float pulse_wait[SKY_STARS];  // a full star's note pulses as light flows through it
    float strike_wait[SKY_TOOLS]; // a string's notes are rate limited
    uint8_t slice_turn;     // the composer works every other frame while a melody plays
    float pad_gain[SKY_STARS], hum_gain[SKY_TOOLS], pad_sent[SKY_STARS], hum_sent[SKY_TOOLS];  // what the voices were last told, and when ago
    float demo_t;           // the new tool's little demonstration
    uint32_t seed;
} tess_sky_t;
