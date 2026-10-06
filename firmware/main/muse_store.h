#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Private account credentials. Never include this record in config/info replies.
#define MUSE_TOKEN_CAP 2048
typedef enum { MUSE_OFF, MUSE_PAIRING, MUSE_PAIRED } muse_state_t;
typedef struct {
    unsigned magic;
    unsigned state;
    unsigned enabled;
    char sdk_token[96];
    char access_token[MUSE_TOKEN_CAP];
    char refresh_token[MUSE_TOKEN_CAP];
} muse_credentials_t;

bool muse_sdk_token_valid(const char *token);
bool muse_account_token_valid(const char *token);
esp_err_t muse_store_load(muse_credentials_t *out);
esp_err_t muse_store_save(const muse_credentials_t *credentials);
esp_err_t muse_store_begin(const char *sdk_token);
muse_state_t muse_store_state(void);
muse_state_t muse_store_saved_state(void);
esp_err_t muse_store_select(bool muse);
void muse_store_wipe(void *data, size_t size);
