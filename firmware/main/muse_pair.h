#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t muse_pair_start(void);
bool muse_pair_key(void);
bool muse_pair_active(void);
