#pragma once

#include "connection_store.h"
#include "nvs.h"

typedef struct { uint8_t digest[32]; } record_digest_t;

bool connection_store_record_contents_valid(const connection_record_t *record);
bool connection_store_record_valid(const connection_record_t *record);
void connection_store_record_defaults(connection_record_t *record);
esp_err_t connection_store_load_legacy(nvs_handle_t handle, connection_record_t *record);
esp_err_t connection_store_digest_record(const connection_record_t *record, record_digest_t *digest);
bool connection_store_digest_matches(const connection_record_t *record, const record_digest_t *digest);
