// Shared by the play core (tess_games.c), her structure (tess_play_shape.c), the stretch and flight
// (tess_stretch.c) and Gather (tess_gather.c).
#pragma once
#include "tess_internal.h"

#define GAME_CX 240.f
#define GAME_CY 255.f

// Discoveries: one per behaviour she can be played into.
enum { BIT_PLUCK, BIT_ECHO, BIT_BLOOM, BIT_TWANG, BIT_BOUNCE, BIT_GATHERED };

bool tess_games_eligible(const face_t *face);
void tess_games_leave(tess_games_t *games);
void tess_games_begin(face_t *face, tess_game_t game);  // a fresh round: clears play and ordinary touch state
void tess_games_earn(face_t *face, unsigned bit);       // a first discovery is marked by a glad flash
void tess_games_celebrate(face_t *face, bool glad);
void tess_games_centre(const face_t *face, float *x, float *y);  // her body's centre on the panel

int tess_play_nearest_vertex(face_t *face, float x, float y, float *distance);  // projected, panel px
void tess_play_pluck(face_t *face, int vertex, bool quiet);
void tess_play_pluck_as(face_t *face, int vertex, bool quiet, float amp, float pace);  // amp < 0 recoils; pace < 1 quick and high
void tess_play_waves(face_t *face, float dt);  // ages waves, sounds their shells
void tess_play_spin(face_t *face, float dt);   // bloom follows her spin

bool tess_stretch_try(face_t *face, float x, float y);  // a finger landed beside her
void tess_stretch_move(face_t *face, float x, float y);
void tess_stretch_release(face_t *face);
void tess_body_update(face_t *face, float dt);          // thrown, rolled by the tilt or sitting: one body
void tess_stretch_update(face_t *face, float dt);
bool tess_fly_tap(face_t *face, float x, float y);       // a tap on her in flight catches her
void tess_self_update(face_t *face, float dt);           // alone, she plays with herself
void tess_self_end(tess_games_t *games);

// Her temper (tess_temper.c).
void tess_temper_update(face_t *face, float dt);
void tess_temper_feel(face_t *face, float excite, float annoy, float fear, float fond);
void tess_temper_pluck(face_t *face, int vertex);         // a tap on her: how she answers this time
void tess_temper_jolt(face_t *face, float jolt);          // the device was knocked: her parts rattle
bool tess_temper_scared(const face_t *face);
uint8_t tess_temper_self_act(face_t *face);

void tess_gather_clear(tess_games_t *games);
void tess_gather_pour(face_t *face, int slot);           // a sliding dot reached her core
bool tess_gather_try_start(face_t *face);              // a finger landed while the burst hangs
void tess_gather_sweep(face_t *face, float x0, float y0, float x1, float y1);
void tess_gather_update(face_t *face, float dt);
void tess_gather_track(face_t *face, float dt);         // the burst's own clock and slots, game or not
