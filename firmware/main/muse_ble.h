#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*muse_ble_command_t)(const char *command, bool encrypted, uint32_t epoch);
esp_err_t muse_ble_start(const char *name, muse_ble_command_t command);
bool muse_ble_send(const char *json, uint32_t record_generation);
bool muse_ble_current(uint32_t epoch);
void muse_ble_disconnect(void);
