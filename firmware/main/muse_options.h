#pragma once
#include <stdbool.h>
#include "cJSON.h"
cJSON *muse_options_reply(const cJSON *request, bool available);
