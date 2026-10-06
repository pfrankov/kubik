#pragma once
#include "app.h"
#include "app_mailbox.h"
bool app_lab_active(void);
bool app_lab_open(void);
void app_lab_close(void);
bool app_lab_event(const app_ev_t *event);
// Called under g_face_mtx by the display task.
bool app_lab_frame(scene_t *scene, float dt, bool powered);

const char *app_lab_screen(void);

// Called under g_face_mtx; returns only real offline collision cues.
bool app_lab_take_cue(tess_cue_t *cue, float *strength, float *position);

void app_lab_play_cue(tess_cue_t cue, float strength, float position);
