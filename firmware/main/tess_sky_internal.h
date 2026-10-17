// Shared by the parts of Skies (tess_sky*.c). Not for the rest of the firmware.
#pragma once
#include "tess_sky.h"

#define SKY_MARGIN 46.f         // stars and tools stay this far from the glass's edge, px
#define SKY_ZONE_R 36.f         // a star lights from particles this near its centre, px
#define SKY_STAR_PULL_R 84.f    // and it gently draws in the right colour from this far
#define SKY_FINGER_R 160.f
#define SKY_GRAB_R 58.f         // a finger this near a tool takes hold of it
#define SKY_CX 240.f
#define SKY_CY 255.f
#define SKY_MAX_SPEED 460.f     // px/s
#define SKY_PATH_N 96
#define SKY_SEQ_RELAY 16       // in a plan's sequence: a relay star, put on the light's path like a tool

// What bends the stream besides the tools: a finger, and the tilt of the cube (screen axes, -1..1: the pour).
typedef struct { bool finger; float fx, fy, gx, gy; } sky_force_t;
// What happened this frame, for the sound: the loudest strike on each string (0..1), the light inside each star, and
// the stars that have just become full (a bit each).
typedef struct { float strike[SKY_TOOLS]; uint8_t inside[SKY_STARS]; uint8_t locked; } sky_hits_t;
// A single light traced through a sky: where it goes, the colour it carries and how far it has gone.
typedef struct { uint8_t n; int16_t x[SKY_PATH_N], y[SKY_PATH_N]; uint8_t color[SKY_PATH_N]; uint16_t arc[SKY_PATH_N]; } sky_path_t;

// The stream (tess_sky_stream.c)
uint32_t sky_rand(sky_world_t *w);           // the sky's own xorshift
float sky_randf(sky_world_t *w);             // 0..1
float sky_range(sky_world_t *w, float low, float high);
void sky_stream_step(sky_world_t *w, sky_particle_t *p, int n, float dt, const sky_force_t *force, sky_hits_t *hits);
void sky_stream_launch(sky_world_t *w, sky_particle_t *p, int n, const int32_t from[][2]);  // let go from places (NULL: from the source)
// Follow one light from the source for `steps` frames, noting its place every `every`. parity: which way a splitter turns it.
void sky_trace(const sky_world_t *w, int parity, const sky_force_t *force, int steps, int every, sky_path_t *path);
bool sky_solved(const sky_world_t *w);       // every star full
void sky_dir_vector(int dir, float *dx, float *dy);   // a relay's way (SKY_DIRS of them)

// Making a sky (tess_sky_compose.c)
void sky_compose(sky_world_t *w, uint32_t seed, const sky_recipe_t *recipe);   // all at once (tests)
// ...or a slice at a time, so that a sky can grow between frames: step until it returns true.
void sky_compose_begin(sky_composer_t *c, sky_world_t *w, uint32_t seed, const sky_recipe_t *recipe);
bool sky_compose_step(sky_composer_t *c);
float sky_degree_hz(int degree);             // D major pentatonic from D4 (degree 0): D E F# A B, then the next octave
void sky_scramble_back(sky_world_t *w);      // every tool, relay and the source where the solution has them (tests)
// What a sky will hold and where its light comes from (tess_sky_plan.c).
void sky_choose_plan(sky_world_t *w, const sky_recipe_t *r, sky_plan_t *pl);
void sky_place_source(sky_world_t *w, const sky_recipe_t *r, bool pour);
int sky_pick(sky_world_t *w, int lo, int hi);
