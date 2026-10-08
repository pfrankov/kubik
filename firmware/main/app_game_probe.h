#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

bool app_game_probe(cJSON *request, char *reply, size_t cap);
