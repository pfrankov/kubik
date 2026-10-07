#include "connection_store_internal.h"

#include <stdio.h>
#include <string.h>
#include "app_nvs.h"
#include "esp_log.h"
#include "nvs.h"


static const char *TAG = "connection_store";

static void cleanup_legacy_keys_locked(void) {
    static const char *const keys[] = {
        "connection", "networks", "wifi", "server", "ssid", "pass", "url", "token", "pin", "muse_v1"
    };
    nvs_handle_t handle;
    if (!app_nvs_ready_locked() || nvs_open("kubik", NVS_READWRITE, &handle) != ESP_OK) return;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        err = nvs_erase_key(handle, keys[i]);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK && app_nvs_recover_locked() != ESP_OK) app_nvs_mark_unhealthy_locked();
}

typedef struct {
    int active_slot;
    bool durable;
} record_source_t;

typedef struct {
    bool present;
    bool valid;
    uint64_t revision;
    record_digest_t digest;
} slot_info_t;

static const char *slot_key(int slot) {
    return slot == 0 ? CONNECTION_RECORD_KEY_A : CONNECTION_RECORD_KEY_B;
}

static esp_err_t read_slot(nvs_handle_t handle, int slot, connection_record_t *record,
                           slot_info_t *info) {
    memset(info, 0, sizeof *info);
    size_t size = sizeof *record;
    esp_err_t err = nvs_get_blob(handle, slot_key(slot), record, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    info->present = true;
    if (err == ESP_ERR_NVS_TYPE_MISMATCH || err == ESP_ERR_NVS_INVALID_LENGTH) return ESP_OK;
    if (err != ESP_OK) return err;
    if (size != sizeof *record || !connection_store_record_valid(record)) return ESP_OK;
    info->valid = true;
    info->revision = record->revision;
    return connection_store_digest_record(record, &info->digest);
}

static esp_err_t select_slot(const slot_info_t *a, const slot_info_t *b, int *selected) {
    if (!a->valid && !b->valid) return ESP_ERR_NVS_NOT_FOUND;
    if (a->valid && b->valid && a->revision == b->revision &&
        memcmp(a->digest.digest, b->digest.digest, sizeof a->digest.digest)) return ESP_ERR_INVALID_STATE;
    *selected = a->valid && (!b->valid || a->revision >= b->revision) ? 0 : 1;
    return ESP_OK;
}

static esp_err_t verify_slot_a(nvs_handle_t handle, connection_record_t *record,
                               const slot_info_t *expected) {
    slot_info_t check;
    esp_err_t err = read_slot(handle, 0, record, &check);
    if (err != ESP_OK) return err;
    bool matches = check.valid && check.revision == expected->revision &&
        !memcmp(check.digest.digest, expected->digest.digest, sizeof check.digest.digest);
    return matches ? ESP_OK : ESP_ERR_INVALID_STATE;
}

// Read both slots and leave the newest valid complete record in `record`.
// An invalid slot can be the interrupted inactive-slot write; it never
// displaces the other valid slot. If neither slot is valid, a present slot
// blocks legacy fallback so old data cannot silently revive over newer data.
static esp_err_t load_slots_locked(nvs_handle_t handle, connection_record_t *record,
                                   record_source_t *source, bool *any_present) {
    slot_info_t a, b;
    esp_err_t err = read_slot(handle, 0, record, &a);
    if (err != ESP_OK) return err;
    err = read_slot(handle, 1, record, &b);
    if (err != ESP_OK) return err;
    *any_present = a.present || b.present;
    int selected;
    err = select_slot(&a, &b, &selected);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    // Slot B was read last. Reload A only when it is the winner, keeping the
    // entire record on the caller's heap allocation rather than task stack.
    if (selected == 0) {
        err = verify_slot_a(handle, record, &a);
        if (err != ESP_OK) return err;
    }
    source->active_slot = selected;
    source->durable = true;
    return ESP_OK;
}

static esp_err_t load_existing_locked(connection_record_t *record, record_source_t *source) {
    source->active_slot = -1;
    source->durable = false;
    if (!app_nvs_ready_locked()) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = nvs_open("kubik", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        connection_store_record_defaults(record);
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    bool any_present = false;
    err = load_slots_locked(handle, record, source, &any_present);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    if (source->durable) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (any_present) {
        nvs_close(handle);
        return ESP_ERR_INVALID_STATE;
    }

    connection_store_record_defaults(record);
    err = connection_store_load_legacy(handle, record);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    if (!connection_store_record_contents_valid(record)) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

// Failed writes are decided only after every application handle is closed and
// the default partition has been deinitialized and initialized again. A/B
// leaves the currently selected slot untouched while the other slot changes.
static esp_err_t write_record_locked(connection_record_t *record,
                                     const record_source_t *old_source,
                                     const record_digest_t *old_digest,
                                     record_source_t *new_source) {
    if (!connection_store_record_valid(record)) return ESP_ERR_INVALID_ARG;
    record_digest_t candidate_digest = {0};
    esp_err_t err = connection_store_digest_record(record, &candidate_digest);
    if (err != ESP_OK) return err;

    int target = old_source->active_slot < 0 ? 0 : 1 - old_source->active_slot;
    nvs_handle_t handle;
    err = nvs_open("kubik", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    esp_err_t write_err = nvs_set_blob(handle, slot_key(target), record, sizeof *record);
    if (write_err == ESP_OK) write_err = nvs_commit(handle);
    nvs_close(handle);
    if (write_err == ESP_OK) {
        new_source->active_slot = target;
        new_source->durable = true;
        return ESP_OK;
    }

    err = app_nvs_recover_locked();
    if (err != ESP_OK) {
        app_nvs_mark_unhealthy_locked();
        return write_err;
    }

    record_source_t recovered_source;
    err = load_existing_locked(record, &recovered_source);
    if (err != ESP_OK) {
        app_nvs_mark_unhealthy_locked();
        return write_err;
    }
    record_digest_t recovered_digest = {0};
    err = connection_store_digest_record(record, &recovered_digest);
    if (err != ESP_OK) {
        app_nvs_mark_unhealthy_locked();
        return write_err;
    }
    if (!memcmp(recovered_digest.digest, candidate_digest.digest, sizeof candidate_digest.digest)) {
        *new_source = recovered_source;
        return ESP_OK;
    }
    if (!memcmp(recovered_digest.digest, old_digest->digest, sizeof old_digest->digest)) {
        *new_source = recovered_source;
        return write_err;
    }

    // A state other than the complete old or candidate record means this
    // partition cannot be safely updated until a later successful recovery.
    app_nvs_mark_unhealthy_locked();
    return write_err;
}

static esp_err_t load_locked(connection_record_t *record, bool *durable) {
    record_source_t source;
    *durable = false;
    esp_err_t err = load_existing_locked(record, &source);
    if (err != ESP_OK) return err;
    if (source.durable) {
        *durable = true;
        cleanup_legacy_keys_locked();
        return ESP_OK;
    }

    record_digest_t old_digest = {0};
    err = connection_store_digest_record(record, &old_digest);
    if (err != ESP_OK) return err;
    record->revision = 1;
    record_source_t migrated_source;
    err = write_record_locked(record, &source, &old_digest, &migrated_source);
    if (err == ESP_OK) {
        *durable = true;
        cleanup_legacy_keys_locked();
        return ESP_OK;
    }
    if (app_nvs_ready_locked() && connection_store_digest_matches(record, &old_digest)) {
        ESP_LOGW(TAG, "connection migration not durable; legacy data remains available");
        *durable = false;
        return ESP_OK;
    }
    return err;
}

typedef esp_err_t (*mutator_t)(connection_record_t *record, const void *context);

typedef enum {
    CONNECTION_PUBLISH_NONE,
    CONNECTION_PUBLISH_RECORD,
    CONNECTION_PUBLISH_PIN,
} connection_publish_t;

static void publish_record(const connection_record_t *record, connection_publish_t mode) {
    if (mode == CONNECTION_PUBLISH_RECORD) settings_apply_connection_record(record);
    else if (mode == CONNECTION_PUBLISH_PIN) settings_apply_connection_pin(record);
}

static esp_err_t update_locked(connection_record_t *record, mutator_t mutate, const void *context,
                                connection_publish_t *publish) {
    record_source_t source;
    esp_err_t err = load_existing_locked(record, &source);
    if (err != ESP_OK) return err;
    record_digest_t old_digest = {0};
    if ((err = connection_store_digest_record(record, &old_digest)) != ESP_OK) return err;
    if (source.durable && record->revision == UINT64_MAX) return ESP_ERR_INVALID_STATE;
    uint64_t next_revision = source.durable ? record->revision + 1 : 1;
    err = mutate(record, context);
    if (err != ESP_OK) return err;
    record->revision = next_revision;
    if (*publish == CONNECTION_PUBLISH_RECORD) record->network_revision = next_revision;
    if (!connection_store_record_valid(record)) return ESP_ERR_INVALID_STATE;

    record_source_t committed_source;
    err = write_record_locked(record, &source, &old_digest, &committed_source);
    if (err == ESP_OK) {
        cleanup_legacy_keys_locked();
        // Publish only fields changed by this operation while still serialized.
        publish_record(record, *publish);
    }
    return err;
}

static esp_err_t update(connection_record_t *record, mutator_t mutate, const void *context,
                        connection_publish_t *publish) {
    if (!record || !mutate || !publish) return ESP_ERR_INVALID_ARG;
    esp_err_t err = app_nvs_lock();
    if (err != ESP_OK) return err;
    if (!app_nvs_ready_locked()) err = ESP_ERR_INVALID_STATE;
    else err = update_locked(record, mutate, context, publish);
    app_nvs_unlock();
    return err;
}

esp_err_t connection_store_load(connection_record_t *record, bool *durable) {
    if (!record || !durable) return ESP_ERR_INVALID_ARG;
    esp_err_t err = app_nvs_lock();
    if (err != ESP_OK) return err;
    if (!app_nvs_ready_locked()) err = ESP_ERR_INVALID_STATE;
    else {
        err = load_locked(record, durable);
    }
    app_nvs_unlock();
    return err;
}

static int find_profile(const connection_networks_t *networks, const char *ssid) {
    for (int i = 0; i < networks->count; i++)
        if (!strcmp(networks->profiles[i].ssid, ssid)) return i;
    return -1;
}

static esp_err_t upsert_network(connection_networks_t *networks, const char *ssid,
                                const char *password, int *active_index) {
    int index = find_profile(networks, ssid);
    char chosen_password[65] = {0};
    if (password[0]) snprintf(chosen_password, sizeof chosen_password, "%s", password);
    else if (index >= 0) snprintf(chosen_password, sizeof chosen_password, "%s", networks->profiles[index].password);
    if (chosen_password[0] && strlen(chosen_password) < 8) return ESP_ERR_INVALID_ARG;
    if (index < 0) {
        if (networks->count == SETTINGS_WIFI_MAX) return ESP_ERR_NO_MEM;
        index = networks->count++;
    }
    snprintf(networks->profiles[index].ssid, sizeof networks->profiles[index].ssid, "%s", ssid);
    snprintf(networks->profiles[index].password, sizeof networks->profiles[index].password, "%s", chosen_password);
    networks->active = index;
    *active_index = index;
    return ESP_OK;
}

typedef struct { const char *ssid, *password, *url; } network_context_t;

static esp_err_t mutate_network(connection_record_t *record, const void *opaque) {
    const network_context_t *context = opaque;
    int active;
    esp_err_t err = upsert_network(&record->networks, context->ssid, context->password, &active);
    if (err != ESP_OK) return err;
    snprintf(record->networks.url, sizeof record->networks.url, "%s", context->url);
    record->flags &= ~CONNECTION_RECORD_PIN_SET;
    memset(record->server_pin, 0, sizeof record->server_pin);
    return ESP_OK;
}

esp_err_t connection_store_save_network(connection_record_t *record,
                                         const char *ssid, const char *password, const char *url) {
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || !password || strlen(password) > 64 ||
        !connection_store_server_valid(url)) return ESP_ERR_INVALID_ARG;
    network_context_t context = {ssid, password, url};
    connection_publish_t publish = CONNECTION_PUBLISH_RECORD;
    return update(record, mutate_network, &context, &publish);
}

typedef struct { const uint8_t *pin; } pin_context_t;

static esp_err_t mutate_pin(connection_record_t *record, const void *opaque) {
    const pin_context_t *context = opaque;
    memcpy(record->server_pin, context->pin, sizeof record->server_pin);
    record->flags |= CONNECTION_RECORD_PIN_SET;
    return ESP_OK;
}

esp_err_t connection_store_save_pin(connection_record_t *record, const uint8_t pin[SETTINGS_PIN_BYTES]) {
    if (!pin) return ESP_ERR_INVALID_ARG;
    pin_context_t context = {pin};
    connection_publish_t publish = CONNECTION_PUBLISH_PIN;
    return update(record, mutate_pin, &context, &publish);
}

static uint32_t next_generation(uint32_t generation) {
    generation++;
    return generation ? generation : 1;
}

typedef struct { const char *sdk_token; } begin_context_t;

static esp_err_t mutate_begin_muse(connection_record_t *record, const void *opaque) {
    const begin_context_t *context = opaque;
    memset(&record->muse, 0, sizeof record->muse);
    record->muse.magic = MUSE_STORE_MAGIC;
    record->muse.state = MUSE_PAIRING;
    record->muse.enabled = 1;
    memcpy(record->muse.sdk_token, context->sdk_token, strlen(context->sdk_token) + 1);
    record->muse_generation = next_generation(record->muse_generation);
    return ESP_OK;
}

esp_err_t connection_store_begin_muse(connection_record_t *record, const char *sdk_token) {
    if (!muse_sdk_token_valid(sdk_token)) return ESP_ERR_INVALID_ARG;
    begin_context_t context = {sdk_token};
    connection_publish_t publish = CONNECTION_PUBLISH_NONE;
    return update(record, mutate_begin_muse, &context, &publish);
}

typedef struct { network_context_t network; bool muse_selected; const char *sdk_token; } setup_context_t;

static esp_err_t mutate_setup(connection_record_t *record, const void *opaque) {
    const setup_context_t *context = opaque;
    esp_err_t err = mutate_network(record, &context->network);
    if (err != ESP_OK) return err;
    if (context->sdk_token && context->sdk_token[0]) {
        begin_context_t begin = {context->sdk_token};
        return mutate_begin_muse(record, &begin);
    }
    if (!record->muse.magic) return context->muse_selected ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
    if (record->muse.enabled != (unsigned)context->muse_selected) {
        record->muse.enabled = context->muse_selected;
    }
    return ESP_OK;
}

esp_err_t connection_store_save_setup(connection_record_t *record,
                                      const char *ssid, const char *password, const char *url,
                                      bool muse_selected, const char *sdk_token) {
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || !password || strlen(password) > 64 ||
        !connection_store_server_valid(url) ||
        (sdk_token && sdk_token[0] && !muse_sdk_token_valid(sdk_token))) return ESP_ERR_INVALID_ARG;
    setup_context_t context = {{ssid, password, url}, muse_selected, sdk_token};
    connection_publish_t publish = CONNECTION_PUBLISH_RECORD;
    return update(record, mutate_setup, &context, &publish);
}

typedef struct { bool enabled; } select_context_t;

static esp_err_t mutate_select_muse(connection_record_t *record, const void *opaque) {
    const select_context_t *context = opaque;
    if (!record->muse.magic) return context->enabled ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
    record->muse.enabled = context->enabled;
    return ESP_OK;
}

esp_err_t connection_store_select_muse(connection_record_t *record, bool enabled) {
    select_context_t context = {enabled};
    connection_publish_t publish = CONNECTION_PUBLISH_NONE;
    return update(record, mutate_select_muse, &context, &publish);
}

typedef struct { const muse_credentials_t *credentials; uint32_t generation; } muse_context_t;

static bool same_sdk(const muse_credentials_t *a, const muse_credentials_t *b) {
    return !strcmp(a->sdk_token, b->sdk_token);
}

static esp_err_t mutate_refresh_muse(connection_record_t *record, const void *opaque) {
    const muse_context_t *context = opaque;
    if (!record->muse.magic || record->muse_generation != context->generation ||
        record->muse.state != MUSE_PAIRED || !same_sdk(&record->muse, context->credentials))
        return ESP_ERR_INVALID_STATE;
    // Refresh only replaces the two account tokens. Preserve the currently
    // selected state even if setup changed it while the HTTP request ran.
    memcpy(record->muse.access_token, context->credentials->access_token, sizeof record->muse.access_token);
    memcpy(record->muse.refresh_token, context->credentials->refresh_token, sizeof record->muse.refresh_token);
    return ESP_OK;
}

esp_err_t connection_store_refresh_muse(connection_record_t *record,
                                         const muse_credentials_t *credentials,
                                         uint32_t expected_generation) {
    if (!credentials || !muse_store_credentials_valid(credentials) || credentials->state != MUSE_PAIRED)
        return ESP_ERR_INVALID_ARG;
    muse_context_t context = {credentials, expected_generation};
    connection_publish_t publish = CONNECTION_PUBLISH_NONE;
    return update(record, mutate_refresh_muse, &context, &publish);
}

typedef struct {
    const muse_credentials_t *credentials;
    uint32_t generation;
    uint64_t network_revision;
    const char *ssid, *password;
    connection_publish_t *publish;
} pair_context_t;

static esp_err_t mutate_commit_pairing(connection_record_t *record, const void *opaque) {
    const pair_context_t *context = opaque;
    if (!record->muse.magic || record->muse_generation != context->generation ||
        record->network_revision != context->network_revision ||
        record->muse.state != MUSE_PAIRING || !same_sdk(&record->muse, context->credentials) ||
        context->credentials->state != MUSE_PAIRED) return ESP_ERR_INVALID_STATE;

    int existing = find_profile(&record->networks, context->ssid);
    bool same = existing >= 0 && record->networks.active == existing &&
        !strcmp(record->networks.profiles[existing].password, context->password);
    if (!same) {
        int active;
        esp_err_t err = upsert_network(&record->networks, context->ssid, context->password, &active);
        if (err != ESP_OK) return err;
        record->flags &= ~CONNECTION_RECORD_PIN_SET;
        memset(record->server_pin, 0, sizeof record->server_pin);
        *context->publish = CONNECTION_PUBLISH_RECORD;
    }
    unsigned enabled = record->muse.enabled;
    memcpy(&record->muse, context->credentials, sizeof record->muse);
    record->muse.enabled = enabled;
    return ESP_OK;
}

esp_err_t connection_store_commit_muse_pairing(connection_record_t *record,
                                                const muse_credentials_t *credentials,
                                                uint32_t expected_generation,
                                                uint64_t expected_network_revision,
                                                const char *ssid, const char *password) {
    if (!credentials || !muse_store_credentials_valid(credentials) || credentials->state != MUSE_PAIRED ||
        !ssid || !ssid[0] || strlen(ssid) > 32 || !password || strlen(password) > 64)
        return ESP_ERR_INVALID_ARG;
    connection_publish_t publish = CONNECTION_PUBLISH_NONE;
    pair_context_t context = {credentials, expected_generation, expected_network_revision,
                              ssid, password, &publish};
    return update(record, mutate_commit_pairing, &context, &publish);
}
