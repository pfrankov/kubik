#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "connection_record.h"

// Every caller supplies one heap-allocated record. These operations never put
// the full credential record on a task stack.
esp_err_t connection_store_load(connection_record_t *record, bool *durable);
esp_err_t connection_store_save_network(connection_record_t *record,
                                         const char *ssid, const char *password, const char *url);
esp_err_t connection_store_save_setup(connection_record_t *record,
                                      const char *ssid, const char *password, const char *url,
                                      bool muse_selected, const char *sdk_token);
esp_err_t connection_store_save_pin(connection_record_t *record,
                                     const uint8_t pin[SETTINGS_PIN_BYTES]);
esp_err_t connection_store_begin_muse(connection_record_t *record, const char *sdk_token);
esp_err_t connection_store_select_muse(connection_record_t *record, bool enabled);
esp_err_t connection_store_refresh_muse(connection_record_t *record,
                                         const muse_credentials_t *credentials,
                                         uint32_t expected_generation);
esp_err_t connection_store_commit_muse_pairing(connection_record_t *record,
                                                const muse_credentials_t *credentials,
                                                uint32_t expected_generation,
                                                uint64_t expected_network_revision,
                                                const char *ssid, const char *password);

bool connection_store_server_valid(const char *url);
bool connection_store_networks_valid(const connection_networks_t *networks);
