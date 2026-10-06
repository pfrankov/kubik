#pragma once
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "face.h"
#include "face_math.h"
#include "face_agent.h"
#include "font.h"
#include "sprite.h"
#include "tess.h"
#define PI 3.14159265f
#define CX 240.f

enum {
    P_EW = 0, P_EH, P_ER, P_SEP, P_EY, P_SCL, P_SCR, P_LTL, P_LTR, P_LAL, P_LAR, P_LBOT, P_SMILE, P_HEART,
    P_LOOKX, P_LOOKY, P_TILT, P_MCURVE, P_MW, P_MOPEN, P_MO, P_MDX, P_CHEEK, P_CR, P_CG, P_CB, P_SLEEPEYE, P_DIM,
};

#define COL_MINT 0x62F0DCu
#define COL_PINK 0xFF6FAEu
#define COL_BLUE 0x78A8FFu
#define COL_ORANGE 0xFF7A4Cu
#define COL_GOLD 0xFFD36Eu
#define COL_GREY 0x7C9C98u
#define COL_REC 0xFF3B30u
#define COL_CHEEK 0xFF5C8Au
#define COL_WHITE 0xFFFFFFu
#define COL_PLUSH 0xF2EADCu
#define COL_INK 0x15171Bu


void base_params(face_params_t *params);
void compute_target(face_t *face);
void face_rub_update(face_t *face, float dt);                // the effort meter of both characters, and Plush's reactions
void face_rub_params(const face_t *face, face_params_t *params);
void body_graph(const body_t *body);
void body_request(face_t *face, body_anim_t animation);
void body_update(face_t *face, float dt);
void card_update(face_t *face, float dt);
void draw_card(face_t *face, scene_t *scene);
extern const char *const k_body_names[BA_COUNT];
static inline bool has_body(const face_t *face) { return face_character(face) == CHARACTER_PLUSH && face->body.id[BA_IDLE] >= 0; }

#define MENU_SWAP 0.45f
#define CRON_X 426.f
#define CRON_Y (480.f - CRON_X)
#define BUBBLE_R 21.f
#define BUBBLE_Y 44.f
#define COL_BUBBLE 0x22292Bu
#define COL_CORAL 0xFF8A7Au
void face_spawn(face_t *face, int kind, float x, float y);
void draw_wifi(scene_t *scene, float x, float y, uint32_t color, float alpha, float phase);
void draw_setup_card(face_t *face, scene_t *scene);
void draw_pair_card(face_t *face, scene_t *scene);
void draw_bubble_icon(scene_t *scene, int icon, float x, float y, float radius, float time, float alpha);
void draw_status(const device_status_t *status, scene_t *scene);
void draw_menu(face_t *face, scene_t *scene);
void draw_menu_fade(face_t *face, scene_t *scene);
void draw_cron(face_t *face, scene_t *scene);
void draw_battery(face_t *face, scene_t *scene, bool menu);
void draw_bubble(face_t *face, scene_t *scene);
uint32_t face_color(face_t *face);
void face_plush_draw(face_t *face, scene_t *scene, uint32_t color);
