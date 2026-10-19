// The parts of Skies that know the face: the rounds, the touch, the sound, the picture (tess_sky*.c).
#pragma once
#include "tess_games_internal.h"
#include "tess_sky_internal.h"
#include "tess_sky_melody.h"

#define SKY_FADE_S 1.6f
#define SKY_MELODY_AT_S .45f    // the resolution's melody begins this long after the sky is solved
#define SKY_CHORD_RING_S 1.7f   // ...and its chord rings this long after its last note

// tess_sky_summon.c: (every frame) the summoning star; true when lifting the cube has answered it with a sky
bool sky_summon_tick(face_t *f, float dt);
void sky_summon_point(const face_t *f, float behind, float *x, float *y); // shared head/trail trajectory
// tess_sky_touch.c: each finger creates its own temporary black hole. True while any finger is on the glass.
bool sky_touch(const face_t *f, tess_sky_t *k, sky_force_t *force, float dt);
// tess_sky_sound.c
void sky_sound_frame(face_t *f, tess_sky_t *k, const sky_hits_t *hits, float dt);
void sky_sound_resolve(face_t *f, tess_sky_t *k);   // the swell that begins a resolution
void sky_sound_melody(face_t *f, tess_sky_t *k, float ms);   // its melody's notes due by ms, then its chord
void sky_sound_silence(face_t *f);                  // held pad and well voices let go before the next sky
void sky_sound_exit(face_t *f);                     // active sky voices only; invitation and agent sounds continue
