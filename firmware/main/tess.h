#pragma once
#include "face.h"

void tess_reset(face_t *face);
void tess_set_mode(face_t *face, face_mode_t previous, face_mode_t mode);
void tess_emotion(face_t *face, emotion_t emotion, float seconds);
void tess_event(face_t *face, face_event_t event, float x, float y);
void tess_update(face_t *face, float dt);
void tess_draw(face_t *face, scene_t *scene);

// Restore once, before display startup. Progress is six finite discoveries;
// render fixtures that do not restore it keep their established adult baseline.
void tess_games_restore(face_t *face, uint8_t progress);
void tess_games_set_available(face_t *face, bool available);
void tess_games_update(face_t *face, float dt);
bool tess_games_event(face_t *face, face_event_t event, float x, float y);
bool tess_games_active(const face_t *face);
// Isolated developer previews only: bypass the gesture, not eligibility.
bool tess_games_start(face_t *face, tess_game_t game, unsigned tier);
