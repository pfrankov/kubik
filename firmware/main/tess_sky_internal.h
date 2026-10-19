// Shared by the parts of Skies (tess_sky*.c). Not for the rest of the firmware.
#pragma once
#include "tess_sky.h"

#define SKY_MARGIN 46.f         // stars stay this far from the glass's edge, px
#define SKY_ZONE_R 36.f         // a star lights from particles this near its centre, px
#define SKY_STAR_PULL_R 84.f    // and it gently draws in the stream from this far
#define SKY_FINGER_R 160.f
#define SKY_CX 240.f
#define SKY_CY 255.f
#define SKY_MAX_SPEED 340.f     // total px/s, including overlapping fields and diagonal tilt
#define SKY_STEP_S (1.f / 60.f)
#define SKY_TILT_SCALE .3f
#define SKY_PATH_N 96

// Tilt and independently fading finger fields; each touch uses the same gravity as a permanent black hole.
typedef struct { float gx, gy; uint8_t count; sky_touch_t touch[SKY_TOUCHES]; } sky_force_t;
// Contacts feed the score-free music; orbit counts include permanent holes and the two temporary fields.
typedef struct { uint8_t inside[SKY_STARS], orbit[SKY_HUMS], locked; } sky_hits_t;
// A single light traced through a sky: where it goes, the colour it carries and how far it has gone.
typedef struct { uint8_t n; int16_t x[SKY_PATH_N], y[SKY_PATH_N]; uint8_t color[SKY_PATH_N]; uint16_t arc[SKY_PATH_N]; } sky_path_t;

// The stream (tess_sky_stream.c)
uint32_t sky_rand(sky_world_t *w);           // the sky's own xorshift
float sky_randf(sky_world_t *w);             // 0..1
float sky_range(sky_world_t *w, float low, float high);
void sky_stream_step(sky_world_t *w, sky_particle_t *p, int n, float dt, const sky_force_t *force, sky_hits_t *hits);
void sky_stream_launch(sky_world_t *w, sky_particle_t *p, int n, const int32_t from[][2]);  // let go from places (NULL: from the source)
// Follow one light through the same fixed-tick physics; the composer places stars on this path.
void sky_trace(const sky_world_t *w, const sky_force_t *force, int steps, int every, sky_path_t *path);
bool sky_solved(const sky_world_t *w);       // every star full
void sky_solution(const sky_world_t *w, sky_force_t *force); // a physically verified input, for composition and diagnostics

// Making a sky (tess_sky_compose.c)
void sky_compose(sky_world_t *w, uint32_t seed, const sky_recipe_t *recipe);   // all at once (tests)
// ...or a slice at a time, so that a sky can grow between frames: step until it returns true.
void sky_compose_begin(sky_composer_t *c, sky_world_t *w, uint32_t seed, const sky_recipe_t *recipe);
bool sky_compose_step(sky_composer_t *c);
float sky_degree_hz(int degree);             // D major pentatonic from D4 (degree 0): D E F# A B, then the next octave
// What a sky will hold and where its light comes from (tess_sky_plan.c).
void sky_choose_plan(sky_world_t *w, const sky_recipe_t *r, sky_plan_t *pl);
void sky_place_source(sky_world_t *w, const sky_recipe_t *r, bool pour);
int sky_pick(sky_world_t *w, int lo, int hi);
