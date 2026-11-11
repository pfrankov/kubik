#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "status_json.h"

bool app_emoji_receive(const cJSON *object, const char *raw, size_t bytes, size_t parsed);
void app_emoji_poll(void);
void app_emoji_info(status_json_t *info);
