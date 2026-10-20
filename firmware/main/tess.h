#pragma once
#include "face.h"

void tess_reset(face_t *face);
void tess_set_mode(face_t *face, face_mode_t previous, face_mode_t mode);
void tess_emotion(face_t *face, emotion_t emotion, float seconds);
void tess_event(face_t *face, face_event_t event, float x, float y);
void tess_update(face_t *face, float dt);
void tess_draw(face_t *face, scene_t *scene);
void tess_motion_sound_gate(face_t *face);
bool tess_motion_sound_allowed(const face_t *face);

// Restore once, before display startup. Progress is six finite discoveries;
// render fixtures that do not restore it keep their established adult baseline.
void tess_games_restore(face_t *face, uint8_t progress);
void tess_games_set_available(face_t *face, bool available);
// Tess Skies' progress, as one number kept on the device (0: a fresh start); `settled` is false while a sky is being played.
void tess_games_restore_sky(face_t *face, uint32_t packed);
uint32_t tess_games_sky_progress(const face_t *face, bool *settled);
bool tess_games_end_sky(face_t *face);  // a short BOOT: the stars fade and she folds back; false when no sky is on
void tess_games_update(face_t *face, float dt);
bool tess_games_event(face_t *face, face_event_t event, float x, float y);
bool tess_games_active(const face_t *face);
bool tess_games_owns_tap(const face_t *face);  // a round is on, or this finger's round just ended
// Isolated developer previews only: bypass the gesture, not eligibility.
bool tess_games_start(face_t *face, tess_preview_t play, unsigned tier);
