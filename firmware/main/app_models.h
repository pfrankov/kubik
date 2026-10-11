#pragma once
#include "cJSON.h"
#include <stddef.h>

// USB-only diagnostic pages: offsets 0/16, at most 16 IDs each.
void app_config_models(cJSON *request, char *reply, size_t cap);
