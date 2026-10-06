#pragma once
#include "face.h"

void tess_reset(face_t *face);
void tess_set_mode(face_t *face, face_mode_t previous, face_mode_t mode);
void tess_emotion(face_t *face, emotion_t emotion, float seconds);
void tess_event(face_t *face, face_event_t event, float x, float y);
void tess_update(face_t *face, float dt);
void tess_draw(face_t *face, scene_t *scene);
