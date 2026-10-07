#include "muse_store.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "connection_store.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"

void muse_store_wipe(void *data, size_t size) { mbedtls_platform_zeroize(data, size); }

bool muse_sdk_token_valid(const char *token) {
    if (!token || strncmp(token, "mgst_", 5)) return false;
    size_t size = strlen(token);
    if (size != 48 || !strchr("AEIMQUYcgkosw048", token[47])) return false;
    for (size_t i = 5; i < size; i++)
        if (!isalnum((unsigned char)token[i]) && token[i] != '_' && token[i] != '-') return false;
    return true;
}

bool muse_account_token_valid(const char *token) {
    if (!token || !token[0] || strnlen(token, MUSE_TOKEN_CAP) >= MUSE_TOKEN_CAP) return false;
    for (const unsigned char *p = (const unsigned char *)token; *p; p++)
        if (*p <= 32 || *p >= 127) return false;
    return true;
}

bool muse_store_credentials_valid(const muse_credentials_t *c) {
    if (!c || c->magic != MUSE_STORE_MAGIC || c->state < MUSE_PAIRING || c->state > MUSE_PAIRED || c->enabled > 1 ||
        !memchr(c->sdk_token, 0, sizeof c->sdk_token) || !muse_sdk_token_valid(c->sdk_token) ||
        !memchr(c->access_token, 0, sizeof c->access_token) ||
        !memchr(c->refresh_token, 0, sizeof c->refresh_token)) return false;
    return c->state != MUSE_PAIRED ||
        (muse_account_token_valid(c->access_token) && muse_account_token_valid(c->refresh_token));
}

esp_err_t muse_store_load_generation(muse_credentials_t *out, uint32_t *generation,
                                     uint64_t *network_revision) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (generation) *generation = 0;
    if (network_revision) *network_revision = 0;
    connection_record_t *record = calloc(1, sizeof *record);
    if (!record) return ESP_ERR_NO_MEM;
    bool durable = false;
    esp_err_t err = connection_store_load(record, &durable);
    if (err == ESP_OK) {
        if (!record->muse.magic) err = ESP_ERR_NVS_NOT_FOUND;
        else if (!muse_store_credentials_valid(&record->muse)) err = ESP_ERR_INVALID_STATE;
        else {
            memcpy(out, &record->muse, sizeof *out);
            if (generation) *generation = record->muse_generation;
            if (network_revision) *network_revision = record->network_revision;
        }
    }
    muse_store_wipe(record, sizeof *record);
    free(record);
    if (err != ESP_OK) {
        muse_store_wipe(out, sizeof *out);
        if (generation) *generation = 0;
        if (network_revision) *network_revision = 0;
    }
    return err;
}

esp_err_t muse_store_load(muse_credentials_t *out) { return muse_store_load_generation(out, NULL, NULL); }

esp_err_t muse_store_refresh_save(const muse_credentials_t *credentials, uint32_t generation) {
    if (!muse_store_credentials_valid(credentials) || credentials->state != MUSE_PAIRED)
        return ESP_ERR_INVALID_ARG;
    connection_record_t *record = calloc(1, sizeof *record);
    if (!record) return ESP_ERR_NO_MEM;
    esp_err_t err = connection_store_refresh_muse(record, credentials, generation);
    muse_store_wipe(record, sizeof *record);
    free(record);
    return err;
}

esp_err_t muse_store_commit_pairing(const muse_credentials_t *credentials, uint32_t generation,
                                    uint64_t network_revision,
                                    const char *ssid, const char *password) {
    if (!muse_store_credentials_valid(credentials) || credentials->state != MUSE_PAIRED)
        return ESP_ERR_INVALID_ARG;
    connection_record_t *record = calloc(1, sizeof *record);
    if (!record) return ESP_ERR_NO_MEM;
    esp_err_t err = connection_store_commit_muse_pairing(record, credentials, generation,
                                                         network_revision, ssid, password);
    muse_store_wipe(record, sizeof *record);
    free(record);
    return err;
}

esp_err_t muse_store_begin(const char *sdk_token) {
    if (!muse_sdk_token_valid(sdk_token)) return ESP_ERR_INVALID_ARG;
    connection_record_t *record = calloc(1, sizeof *record);
    if (!record) return ESP_ERR_NO_MEM;
    esp_err_t err = connection_store_begin_muse(record, sdk_token);
    muse_store_wipe(record, sizeof *record);
    free(record);
    return err;
}

static muse_state_t stored_state(bool selected_only) {
    connection_record_t *record = calloc(1, sizeof *record);
    if (!record) return MUSE_OFF;
    muse_state_t state = MUSE_OFF;
    bool durable = false;
    if (connection_store_load(record, &durable) == ESP_OK &&
        record->muse.magic && muse_store_credentials_valid(&record->muse) &&
        (!selected_only || record->muse.enabled)) state = record->muse.state;
    muse_store_wipe(record, sizeof *record);
    free(record);
    return state;
}

muse_state_t muse_store_state(void) { return stored_state(true); }
muse_state_t muse_store_saved_state(void) { return stored_state(false); }

esp_err_t muse_store_select(bool muse) {
    connection_record_t *record = calloc(1, sizeof *record);
    if (!record) return ESP_ERR_NO_MEM;
    esp_err_t err = connection_store_select_muse(record, muse);
    muse_store_wipe(record, sizeof *record);
    free(record);
    return err;
}
