#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum { BLE_RUNTIME_NONE, BLE_RUNTIME_MUSE, BLE_RUNTIME_PRINTER } ble_runtime_owner_t;
typedef esp_err_t (*ble_runtime_prepare_t)(void);

// One controller and host task for the mutually exclusive Muse / printer roles.
esp_err_t ble_runtime_start(ble_runtime_owner_t owner, ble_runtime_prepare_t prepare, void (*sync)(void));
// A timed-out shutdown retains ownership; a second host must never be started.
esp_err_t ble_runtime_stop(ble_runtime_owner_t owner);
bool ble_runtime_owned(ble_runtime_owner_t owner);
