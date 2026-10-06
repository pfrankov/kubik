#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "voice_mode.h"

#define AGENT_MODEL_ID_MAX 160
#define AGENT_MODEL_LABEL_MAX 80
#define AGENT_META_MAX 96
#define AGENT_PAGE_SIZE 4
#define AGENT_ERROR_MAX 23
#define AGENT_REQUEST_TIMEOUT_MS 8000u

typedef struct {
    char id[AGENT_MODEL_ID_MAX + 1];
    char label[AGENT_MODEL_LABEL_MAX + 1];
    bool disabled;
} agent_model_t;

typedef enum {
    AGENT_REQUEST_NONE = 0,
    AGENT_REQUEST_OPTIONS,
    AGENT_REQUEST_SELECT,
} agent_request_kind_t;

typedef enum {
    AGENT_VIEW_OVERVIEW = 0,
    AGENT_VIEW_MODELS,
    AGENT_VIEW_GUIDE,
    AGENT_VIEW_VOICE_MODES,
    AGENT_VIEW_CLASSIC,
} agent_view_t;

typedef enum { AGENT_TARGET_AGENT, AGENT_TARGET_STT, AGENT_TARGET_TTS, AGENT_TARGET_MODE, AGENT_TARGET_VOICE } agent_target_t;
const char *agent_target_name(agent_target_t target);
bool agent_target_parse(const char *name, agent_target_t *target);

typedef struct {
    agent_target_t target;
    uint16_t rid;
    uint8_t cursor, total, count;
    bool has_catalog, has_model;
    agent_model_t models[AGENT_PAGE_SIZE];
    char model[AGENT_MODEL_ID_MAX + 1];
    bool has_stt, stt_available;
    bool has_tts, tts_available;
    char stt_provider[AGENT_META_MAX + 1], stt_model[AGENT_META_MAX + 1];
    char tts_provider[AGENT_META_MAX + 1], tts_model[AGENT_META_MAX + 1];
    char error[AGENT_ERROR_MAX + 1];
} agent_menu_reply_t;

typedef struct {
    bool open, online, options_loaded, request_pending;
    agent_view_t view;
    agent_target_t target;
    char selected_model[AGENT_MODEL_ID_MAX + 1];
    voice_mode_t voice_mode;
    uint8_t half;  // two large choices per screen; wire catalogs contain four
    uint8_t guide_step, volume;
    bool guide_save_failed;
    float guide_elapsed;
    int16_t touch_x, touch_y;
    bool touch_moved;
    bool capabilities_known, stt_available, tts_available;
    uint8_t cursor, total, count;
    agent_model_t models[AGENT_PAGE_SIZE];
    char model[AGENT_MODEL_ID_MAX + 1];
    char stt_provider[AGENT_META_MAX + 1], stt_model[AGENT_META_MAX + 1];
    char tts_provider[AGENT_META_MAX + 1], tts_model[AGENT_META_MAX + 1];
    char error[AGENT_ERROR_MAX + 1];
    uint16_t next_rid, waiting_rid, wire_rid;
    uint32_t request_at_ms, wire_at_ms;
    agent_request_kind_t waiting_kind, retry_kind;
    uint8_t retry_cursor, waiting_cursor;
    char retry_id[AGENT_MODEL_ID_MAX + 1];
    int8_t pressed;
    float pressed_t;
} agent_menu_t;

void agent_menu_reset(agent_menu_t *menu);
void agent_menu_show(agent_menu_t *menu, bool online);
void agent_menu_hide(agent_menu_t *menu);
void agent_menu_set_online(agent_menu_t *menu, bool online);
void agent_menu_set_capabilities(agent_menu_t *menu, bool stt_available, bool tts_available);
uint16_t agent_menu_request_options(agent_menu_t *menu, uint8_t cursor, uint32_t now_ms);
uint16_t agent_menu_request_model(agent_menu_t *menu, const char *id, uint32_t now_ms);
uint16_t agent_menu_retry(agent_menu_t *menu, uint32_t now_ms);
bool agent_menu_accept_reply(agent_menu_t *menu, const agent_menu_reply_t *reply, uint32_t now_ms);
bool agent_menu_timeout(agent_menu_t *menu, uint32_t now_ms);
bool agent_menu_id_valid(const char *id);
bool agent_menu_error_valid(const char *error);
bool agent_menu_current(const agent_menu_t *menu, unsigned index);
unsigned agent_menu_pages(const agent_menu_t *menu);
unsigned agent_menu_display_pages(const agent_menu_t *menu);
unsigned agent_menu_display_page(const agent_menu_t *menu);
bool agent_menu_can_turn(const agent_menu_t *menu, bool next);
// Changes the local half; true means load the returned wire catalog page.
bool agent_menu_turn_page(agent_menu_t *menu, bool next, uint8_t *cursor);
bool agent_menu_drag(agent_menu_t *menu, int x, int y, bool first);
bool agent_menu_take_tap(agent_menu_t *menu);
void agent_menu_start_guide(agent_menu_t *menu, bool online);
void agent_menu_guide_move(agent_menu_t *menu, int direction);

// Page transitions clear stale catalogs and pending requests; no network side effects.
void agent_menu_visit(agent_menu_t *menu, agent_view_t view, agent_target_t target);
void agent_menu_back(agent_menu_t *menu);
void agent_menu_voice_models(agent_menu_t *menu);
