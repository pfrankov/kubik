#pragma once
#include <stddef.h>
#include "cJSON.h"

// Strict, bounded JSON for credentials and control; reject embedded NULs.
cJSON *muse_json_parse(const char *bytes, size_t size);
void muse_json_clear(cJSON *object);
