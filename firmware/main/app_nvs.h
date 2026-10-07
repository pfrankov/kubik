#pragma once

#include <stdbool.h>
#include "esp_err.h"

// All application access to the default NVS partition is serialized through
// this lock. Recovery is only called while the caller holds it and has closed
// every NVS handle it opened.
esp_err_t app_nvs_lock(void);
void app_nvs_unlock(void);
esp_err_t app_nvs_init_locked(void);
esp_err_t app_nvs_recover_locked(void);
bool app_nvs_ready_locked(void);
void app_nvs_mark_unhealthy_locked(void);
