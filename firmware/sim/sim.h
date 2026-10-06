// Shared by the simulator's scenario files: the renderer front end living in sim.c.
#pragma once
#include <stdint.h>

#include "../main/face.h"

#define PANEL_W (R_W * R_SCALE)
#define PANEL_H (R_H * R_SCALE)

extern uint16_t panel[PANEL_H][PANEL_W];

void sim_mood_setup(face_t *f);  // SIM_MOOD_OFF: a neutral Tess (call before any event)
void sim_render(face_t *f);  // face_draw + the device's render path into `panel`
void sim_render_reset(void); // the renderer starts from nothing, for an unrelated scene
void sim_emit_frame(void);   // panel as one rgb24 frame on stdout
void sim_to_rgb(uint16_t c, unsigned char *o);
void sim_sheet(bool per_tile);  // the contact sheet, or one frame per tile
void sim_gravity(void);         // Tess rolling under a slowly turning gravity
void sim_rotate(void);          // Tess while the device is turned about a changing axis
void sim_jitter(void);          // Tess under uneven frame times, checked for sane motion
void sim_gaze(const char *name);  // Tess's attention, one phase forced at a time; `list` names them
void sim_mood(const char *name);  // Tess's idle moves and reactions; NULL: all of them
void sim_feel(const char *name);  // Tess's moods: a clip each, a sheet of all of them, the changes between them; NULL: every clip
void sim_play(const char *name);  // what a finger does to Tess, and what it does alone; `list` names them
void sim_rub(const char *name, int character);  // petting scripts; NULL: all of the reference stream
void sim_text(void);  // every screen with text, one settled frame each

void sim_dialogue(void);  // idle -> thinking -> voiced reply with pauses -> return

void sim_live_dialogue(void); // persistent Live with microphone/playback handoffs

void sim_load_assets(void);
void sim_render_scene(scene_t *scene);
