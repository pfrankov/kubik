#include "muse_store.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "mbedtls/platform_util.h"

#define MUSE_MAGIC 0x4d555301u

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

static bool valid_credentials(const muse_credentials_t *c) {
    if (c->magic != MUSE_MAGIC || c->state < MUSE_PAIRING || c->state > MUSE_PAIRED || c->enabled > 1 ||
        !memchr(c->sdk_token, 0, sizeof c->sdk_token) || !muse_sdk_token_valid(c->sdk_token) ||
        !memchr(c->access_token, 0, sizeof c->access_token) ||
        !memchr(c->refresh_token, 0, sizeof c->refresh_token)) return false;
    return c->state != MUSE_PAIRED ||
        (muse_account_token_valid(c->access_token) && muse_account_token_valid(c->refresh_token));
}

esp_err_t muse_store_load(muse_credentials_t *out) {
    memset(out, 0, sizeof *out);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("kubik", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t size = sizeof *out;
    err = nvs_get_blob(handle, "muse_v1", out, &size);
    nvs_close(handle);
    if (err == ESP_OK && (size != sizeof *out || !valid_credentials(out))) err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) muse_store_wipe(out, sizeof *out);
    return err;
}

esp_err_t muse_store_save(const muse_credentials_t *credentials) {
    if (!valid_credentials(credentials)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("kubik", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "muse_v1", credentials, sizeof *credentials);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t muse_store_begin(const char *sdk_token) {
    if (!muse_sdk_token_valid(sdk_token)) return ESP_ERR_INVALID_ARG;
    muse_credentials_t *credentials = calloc(1, sizeof *credentials);
    if (!credentials) return ESP_ERR_NO_MEM;
    credentials->magic = MUSE_MAGIC;
    credentials->state = MUSE_PAIRING;
    credentials->enabled = 1;
    memcpy(credentials->sdk_token, sdk_token, strlen(sdk_token) + 1);
    esp_err_t err = muse_store_save(credentials);
    muse_store_wipe(credentials, sizeof *credentials);
    free(credentials);
    return err;
}

static muse_state_t stored_state(bool selected_only) {
    muse_credentials_t *credentials = calloc(1, sizeof *credentials);
    if (!credentials) return MUSE_OFF;
    muse_state_t state = MUSE_OFF;
    if (muse_store_load(credentials) == ESP_OK && (!selected_only || credentials->enabled)) state = credentials->state;
    muse_store_wipe(credentials, sizeof *credentials); free(credentials); return state;
}
muse_state_t muse_store_state(void) { return stored_state(true); }
muse_state_t muse_store_saved_state(void) { return stored_state(false); }

esp_err_t muse_store_select(bool muse) {
    muse_credentials_t *credentials = calloc(1, sizeof *credentials);
    if (!credentials) return ESP_ERR_NO_MEM;
    esp_err_t err = muse_store_load(credentials);
    if (err == ESP_OK) { credentials->enabled = muse; err = muse_store_save(credentials); }
    else if (!muse && err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    muse_store_wipe(credentials, sizeof *credentials); free(credentials); return err;
}
