#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t muse_pair_start(void);
bool muse_pair_key(void);
// Consumes the shared BOOT only while the secure physical confirmation is pending.
bool muse_pair_confirm_button(void);
bool muse_pair_active(void);
