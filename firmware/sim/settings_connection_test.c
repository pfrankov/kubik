#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "../main/settings.h"
#include "../main/muse_store.h"
#include "../main/connection_record.h"
#include "../main/connection_store.h"

static const char *latest_record_key(void) {
    connection_record_t a, b;
    size_t a_size = sizeof a, b_size = sizeof b;
    esp_err_t a_err = nvs_get_blob(1, CONNECTION_RECORD_KEY_A, &a, &a_size);
    esp_err_t b_err = nvs_get_blob(1, CONNECTION_RECORD_KEY_B, &b, &b_size);
    bool a_valid = a_err == ESP_OK && a_size == sizeof a;
    bool b_valid = b_err == ESP_OK && b_size == sizeof b;
    assert(a_valid || b_valid);
    if (!a_valid) return CONNECTION_RECORD_KEY_B;
    if (!b_valid) return CONNECTION_RECORD_KEY_A;
    return a.revision >= b.revision ? CONNECTION_RECORD_KEY_A : CONNECTION_RECORD_KEY_B;
}

static void fill_sdk_token(char token[49], char fill) {
    memcpy(token, "mgst_", 5);
    memset(token + 5, fill, 43);
    token[48] = 0;
}

static void paired_credentials(muse_credentials_t *credentials) {
    credentials->state = MUSE_PAIRED;
    snprintf(credentials->access_token, sizeof credentials->access_token, "access-token");
    snprintf(credentials->refresh_token, sizeof credentials->refresh_token, "refresh-token");
}

static void test_runtime_fallback_reads_and_writes(void) {
    nvs_flash_erase();
    settings_load();
    assert(settings_save_connection("preferred-A", "preferred-password", "") == ESP_OK);
    assert(settings_save_connection("fallback-B", "fallback-password", "") == ESP_OK);

    connection_record_t saved;
    size_t size = sizeof saved;
    assert(nvs_get_blob(1, latest_record_key(), &saved, &size) == ESP_OK && size == sizeof saved);
    saved.networks.active = 0;
    assert(nvs_set_blob(1, latest_record_key(), &saved, sizeof saved) == ESP_OK);
    settings_load();
    assert(!strcmp(g_settings.wifi_ssid, "preferred-A"));

    char sdk_token[49];
    fill_sdk_token(sdk_token, 'A');
    assert(muse_store_begin(sdk_token) == ESP_OK);
    muse_credentials_t credentials;
    uint32_t generation;
    uint64_t network_revision;
    assert(muse_store_load_generation(&credentials, &generation, &network_revision) == ESP_OK);
    paired_credentials(&credentials);
    assert(muse_store_commit_pairing(&credentials, generation, network_revision,
                                     "preferred-A", "preferred-password") == ESP_OK);

    snprintf(g_settings.wifi_ssid, sizeof g_settings.wifi_ssid, "%s", "fallback-B");
    snprintf(g_settings.wifi_pass, sizeof g_settings.wifi_pass, "%s", "fallback-password");
    connection_record_t *record = calloc(1, sizeof *record);
    assert(record);
    bool durable = false;
    assert(connection_store_load(record, &durable) == ESP_OK && durable);
    assert(muse_store_state() == MUSE_PAIRED && muse_store_saved_state() == MUSE_PAIRED);
    snprintf(credentials.access_token, sizeof credentials.access_token, "%s", "access-refreshed");
    snprintf(credentials.refresh_token, sizeof credentials.refresh_token, "%s", "refresh-refreshed");
    assert(muse_store_refresh_save(&credentials, generation) == ESP_OK);
    assert(!strcmp(g_settings.wifi_ssid, "fallback-B") &&
           !strcmp(g_settings.wifi_pass, "fallback-password"));
    muse_store_wipe(record, sizeof *record);
    free(record);

    uint8_t pin[SETTINGS_PIN_BYTES] = {0x5a};
    assert(settings_save_pin(pin) == ESP_OK && g_settings.server_pinned);
    assert(!strcmp(g_settings.wifi_ssid, "fallback-B") &&
           !strcmp(g_settings.wifi_pass, "fallback-password"));
    assert(settings_save_connection("preferred-A", "preferred-password", "") == ESP_OK);
    assert(!strcmp(g_settings.wifi_ssid, "preferred-A") &&
           !strcmp(g_settings.wifi_pass, "preferred-password"));
    muse_store_wipe(&credentials, sizeof credentials);
    muse_store_wipe(&saved, sizeof saved);
}

static void test_pairing_preserves_newer_intent(void) {
    nvs_flash_erase();
    settings_load();
    assert(settings_save_connection("preferred-A", "preferred-password", "") == ESP_OK);

    char sdk_token[49];
    fill_sdk_token(sdk_token, 'A');
    assert(muse_store_begin(sdk_token) == ESP_OK);
    muse_credentials_t credentials;
    uint32_t generation;
    uint64_t network_revision;
    assert(muse_store_load_generation(&credentials, &generation, &network_revision) == ESP_OK);
    paired_credentials(&credentials);

    // Selection is mutable UI state, not a different account generation. Pairing
    // may complete after it changes, and must preserve the current disabled flag.
    assert(muse_store_select(false) == ESP_OK);
    assert(muse_store_commit_pairing(&credentials, generation, network_revision,
                                     "preferred-A", "preferred-password") == ESP_OK);
    assert(muse_store_state() == MUSE_OFF && muse_store_saved_state() == MUSE_PAIRED);

    fill_sdk_token(sdk_token, 'E');
    assert(muse_store_begin(sdk_token) == ESP_OK);
    assert(muse_store_load_generation(&credentials, &generation, &network_revision) == ESP_OK);
    paired_credentials(&credentials);
    assert(settings_save_connection("newer-B", "newer-password", "") == ESP_OK);
    assert(muse_store_commit_pairing(&credentials, generation, network_revision,
                                     "preferred-A", "preferred-password") == ESP_ERR_INVALID_STATE);
    assert(!strcmp(g_settings.wifi_ssid, "newer-B") && muse_store_saved_state() == MUSE_PAIRING);
    muse_store_wipe(&credentials, sizeof credentials);
}

static void test_pairing_rejects_same_network_intent(void) {
    nvs_flash_erase();
    settings_load();
    assert(settings_save_connection("preferred-A", "preferred-password", "") == ESP_OK);
    char sdk_token[49];
    fill_sdk_token(sdk_token, 'A');
    assert(muse_store_begin(sdk_token) == ESP_OK);
    muse_credentials_t credentials;
    uint32_t generation;
    uint64_t network_revision;
    assert(muse_store_load_generation(&credentials, &generation, &network_revision) == ESP_OK);
    paired_credentials(&credentials);

    // A same-value save records a newer user intent despite identical content.
    assert(settings_save_connection("preferred-A", "preferred-password", "") == ESP_OK);
    assert(muse_store_commit_pairing(&credentials, generation, network_revision,
                                     "preferred-A", "preferred-password") == ESP_ERR_INVALID_STATE);
    assert(muse_store_saved_state() == MUSE_PAIRING && !strcmp(g_settings.wifi_ssid, "preferred-A"));
    muse_store_wipe(&credentials, sizeof credentials);
}

static void test_pairing_selects_known_inactive_network(void) {
    nvs_flash_erase();
    settings_load();
    assert(settings_save_connection("preferred-A", "preferred-password", "kubik://server.example") == ESP_OK);
    assert(settings_save_connection("known-B", "known-password", "kubik://server.example") == ESP_OK);

    connection_record_t record;
    size_t size = sizeof record;
    assert(nvs_get_blob(1, latest_record_key(), &record, &size) == ESP_OK && size == sizeof record);
    record.networks.active = 0;
    assert(nvs_set_blob(1, latest_record_key(), &record, sizeof record) == ESP_OK);
    settings_load();
    assert(!strcmp(g_settings.wifi_ssid, "preferred-A"));

    char sdk_token[49];
    fill_sdk_token(sdk_token, 'I');
    assert(muse_store_begin(sdk_token) == ESP_OK);
    muse_credentials_t credentials;
    uint32_t generation;
    uint64_t network_revision;
    assert(muse_store_load_generation(&credentials, &generation, &network_revision) == ESP_OK);
    paired_credentials(&credentials);
    assert(muse_store_commit_pairing(&credentials, generation, network_revision,
                                     "known-B", "known-password") == ESP_OK);
    assert(!strcmp(g_settings.wifi_ssid, "known-B") &&
           !strcmp(g_settings.server_url, "kubik://server.example"));
    muse_store_wipe(&credentials, sizeof credentials);
    muse_store_wipe(&record, sizeof record);
}

void settings_test_connection_runtime(void) {
    test_runtime_fallback_reads_and_writes();
    test_pairing_preserves_newer_intent();
    test_pairing_rejects_same_network_intent();
    test_pairing_selects_known_inactive_network();
}
