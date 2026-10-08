#pragma once
#include "face.h"
#include "qr.h"
#include "menu_service.h"

typedef enum {
    LAB_HOME, LAB_CONNECT, LAB_WIFI_QR, LAB_SETUP_QR, LAB_PAIR,
    LAB_SETTINGS, LAB_SOUND, LAB_AGENT, LAB_VOICE, LAB_MODELS, LAB_GUIDE,
    LAB_RECORD, LAB_THINK, LAB_SPEAK, LAB_LIVE, LAB_TEXT, LAB_OFFLINE, LAB_ERROR,
    LAB_BACKGROUND, LAB_REMINDER, LAB_EVENTS, LAB_EVENT_LOG,
    LAB_GROW_POINT, LAB_GROW_SQUARE, LAB_GROW_CUBE, LAB_GROW_TESSERACT,
    LAB_ECHO_0, LAB_ECHO_1, LAB_ECHO_2, LAB_CATCH_0, LAB_CATCH_1, LAB_CATCH_2,
    LAB_COUNT
} lab_screen_t;
typedef enum { LAB_TAP, LAB_BOOT, LAB_KEY, LAB_MENU, LAB_POWER, LAB_HOLD } lab_event_t;
typedef struct {
    face_t face;
    uint8_t qr[QR_MAX_N * QR_MAX_N];
    rub_track_t finger;
    menu_service_t service;
    int selected, page, character;
    bool signal, overlay;
    int volume, ui_volume, brightness;
    uint8_t model_choice[5], event_step;
} screen_lab_t;
const char *screen_lab_name(int screen);
typedef struct { int taps; int64_t first_ms; bool ready; } screen_lab_unlock_t;
bool screen_lab_unlock_tap(screen_lab_unlock_t *state, int x, int y, int64_t now);
bool screen_lab_entry(bool unlocked, int x, int y);
void screen_lab_catalog(screen_lab_t *lab);
void screen_lab_init(screen_lab_t *lab, int character);
void screen_lab_select(screen_lab_t *lab, int screen);
void screen_lab_event(screen_lab_t *lab, lab_event_t event, int x, int y);
void screen_lab_pointer(screen_lab_t *lab, bool down, int x, int y);
void screen_lab_update(screen_lab_t *lab, float dt);
void screen_lab_draw(screen_lab_t *lab, scene_t *scene);
bool screen_lab_controls(const screen_lab_t *lab);
int screen_lab_control_y(const screen_lab_t *lab);

bool screen_lab_take_cue(screen_lab_t *lab, tess_cue_t *cue, float *strength, float *position);

void screen_lab_demo_event(screen_lab_t *lab);
void screen_lab_event_fixture(screen_lab_t *lab, int screen);
