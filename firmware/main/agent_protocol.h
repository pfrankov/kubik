#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "agent_menu.h"
#include "cJSON.h"

bool agent_protocol_parse_options(cJSON *json, agent_menu_reply_t *reply);
bool agent_protocol_parse_capabilities(cJSON *json, bool *stt, bool *tts);
bool agent_protocol_has_nul_escape(const char *json, size_t len);
