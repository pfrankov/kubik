#pragma once

#include "agent_menu.h"
#include "render.h"

typedef enum {
    AGENT_HIT_NONE = 0,
    AGENT_HIT_BACK,
    AGENT_HIT_PREV,
    AGENT_HIT_NEXT,
    AGENT_HIT_RETRY,
    AGENT_HIT_MODELS,
    AGENT_HIT_VOICE,
    AGENT_HIT_STT,
    AGENT_HIT_TTS,
    AGENT_HIT_REFRESH,
    AGENT_HIT_MODEL_0,
    AGENT_HIT_MODEL_1,
    AGENT_HIT_MODEL_2,
    AGENT_HIT_MODEL_3,
    AGENT_HIT_GUIDE_NEXT,
    AGENT_HIT_GUIDE_SKIP,
    AGENT_HIT_GUIDE_DONE,
} face_agent_hit_t;

void face_agent_draw(scene_t *scene, const agent_menu_t *menu, bool is_tess);
face_agent_hit_t face_agent_hit(const agent_menu_t *menu, int x, int y);
