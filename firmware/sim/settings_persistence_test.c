#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "../main/settings.h"
#include "../main/muse_store.h"
#include "../main/connection_record.h"

extern const char *fail_write, *fail_read;

static bool key_present(const char *key) {
    unsigned char buffer[6000];
    size_t size = sizeof buffer;
    return nvs_get_blob(1, key, buffer, &size) == ESP_OK;
}

static bool connection_present(void) {
    return key_present(CONNECTION_RECORD_KEY_A) || key_present(CONNECTION_RECORD_KEY_B);
}

static const char *newest_record_key(void) {
    connection_record_t a, b;
    size_t a_size = sizeof a, b_size = sizeof b;
    bool a_ok = nvs_get_blob(1, CONNECTION_RECORD_KEY_A, &a, &a_size) == ESP_OK;
    bool b_ok = nvs_get_blob(1, CONNECTION_RECORD_KEY_B, &b, &b_size) == ESP_OK;
    assert(a_ok || b_ok);
    if (!a_ok) return CONNECTION_RECORD_KEY_B;
    if (!b_ok) return CONNECTION_RECORD_KEY_A;
    return a.revision >= b.revision ? CONNECTION_RECORD_KEY_A : CONNECTION_RECORD_KEY_B;
}

static const char *inactive_record_key(void) {
    return !strcmp(newest_record_key(), CONNECTION_RECORD_KEY_A)
        ? CONNECTION_RECORD_KEY_B : CONNECTION_RECORD_KEY_A;
}

static void test_server_forms_and_pin(void) {
    const char *valid[] = {"", "kubik://kubik-host.local", "kubik://192.168.1.5:18790", "kubik://[fe80::1]:18790",
        "wss://example.com/kubik/v1", "wss://10.0.0.2:18790/kubik/v1"};
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; i++) assert(settings_server_valid(valid[i]));
    const char *invalid[] = {"kubik://", "kubik://host/", "kubik://host/kubik/v1", "kubik://host:0",
        "kubik://host:65536", "kubik://user@host", "kubik://[::1]x", "kubik://bad..host", "KUBIK://host", " "};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) assert(!settings_server_valid(invalid[i]));

    nvs_flash_erase();
    assert(settings_save_connection("home", "home-password", "") == ESP_OK);
    assert(!g_settings.server_pinned);
    uint8_t pin[SETTINGS_PIN_BYTES];
    memset(pin, 0xa5, sizeof pin);
    assert(settings_save_pin(pin) == ESP_OK && g_settings.server_pinned);
    settings_load();
    assert(g_settings.server_pinned && !memcmp(g_settings.server_pin, pin, sizeof pin));
    fail_write = inactive_record_key();
    assert(settings_save_connection("home", "home-password", "kubik://other.local") == ESP_FAIL);
    fail_write = NULL;
    settings_load();
    assert(g_settings.server_pinned && !strcmp(g_settings.server_url, ""));
    assert(settings_save_connection("home", "home-password", "") == ESP_OK);
    assert(!g_settings.server_pinned && !key_present("pin"));
    assert(settings_save_pin(pin) == ESP_OK);
    assert(settings_save_connection("home", "home-password", "kubik://other.local:18790") == ESP_OK);
    settings_load();
    assert(!g_settings.server_pinned && !strcmp(g_settings.server_url, "kubik://other.local:18790"));

    connection_record_t saved;
    size_t size = sizeof saved;
    assert(nvs_get_blob(1, newest_record_key(), &saved, &size) == ESP_OK && size == sizeof saved);
    saved.flags &= ~CONNECTION_RECORD_PIN_SET;
    memset(saved.server_pin, 0, sizeof saved.server_pin);
    assert(nvs_set_blob(1, newest_record_key(), &saved, sizeof saved) == ESP_OK);
    settings_load();
    assert(!g_settings.server_pinned);
}

static void test_current_connection_migration(void) {
    typedef struct { char ssid[33], password[65], url[128]; } old_connection_t;
    nvs_flash_erase();
    old_connection_t old = {0};
    snprintf(old.ssid, sizeof old.ssid, "current-net");
    snprintf(old.password, sizeof old.password, "current-password");
    snprintf(old.url, sizeof old.url, "wss://current/kubik/v1");
    const uint8_t pin[32] = {0x6b};
    nvs_set_blob(1, "connection", &old, sizeof old);
    nvs_set_blob(1, "pin", pin, sizeof pin);
    settings_load();
    assert(g_settings.wifi_profile_count == 1 && !strcmp(g_settings.wifi_ssid, "current-net"));
    assert(!strcmp(g_settings.wifi_profiles[0].password, "current-password"));
    assert(g_settings.server_pinned && !memcmp(g_settings.server_pin, pin, sizeof pin));
    assert(connection_present() && !key_present("connection") && !key_present("pin"));
}

static void test_open_network_string_migration(void) {
    nvs_flash_erase();
    assert(nvs_set_str(1, "ssid", "open-network") == ESP_OK);
    settings_load();
    assert(g_settings.wifi_profile_count == 1 && !strcmp(g_settings.wifi_ssid, "open-network"));
    assert(!g_settings.wifi_pass[0] && connection_present() && !key_present("ssid"));
    settings_load();
    assert(g_settings.wifi_profile_count == 1 && !g_settings.wifi_pass[0]);

    for (int empty_ssid = 0; empty_ssid < 2; empty_ssid++) {
        nvs_flash_erase();
        if (empty_ssid) assert(nvs_set_str(1, "ssid", "") == ESP_OK);
        assert(nvs_set_str(1, "pass", "orphan-password") == ESP_OK);
        settings_load();
        assert(!g_settings.wifi_profile_count && !g_settings.wifi_ssid[0]);
        assert(!g_settings.wifi_profiles[0].password[0]);
    }

    nvs_flash_erase();
    assert(nvs_set_str(1, "ssid", "unreadable-password") == ESP_OK);
    fail_read = "pass";
    settings_load();
    fail_read = NULL;
    assert(!connection_present() && key_present("ssid") && !g_settings.wifi_profile_count);
}

// Exact pre-PR layout, including fields outside the connection record.
typedef struct {
    uint32_t magic;
    uint8_t version, count, active, reserved;
    wifi_profile_t profiles[SETTINGS_WIFI_MAX];
    char url[128];
} main_networks_t;

static void assert_main_networks(const main_networks_t *old, const uint8_t pin[32]) {
    assert(g_settings.wifi_profile_count == old->count && !strcmp(g_settings.wifi_ssid, "second"));
    assert(!memcmp(g_settings.wifi_profiles, old->profiles, sizeof old->profiles));
    assert(!strcmp(g_settings.server_url, old->url) && g_settings.server_pinned);
    assert(!memcmp(g_settings.server_pin, pin, 32));
}

static void assert_main_muse(const muse_credentials_t *muse) {
    muse_credentials_t loaded;
    assert(muse_store_load(&loaded) == ESP_OK && !memcmp(&loaded, muse, sizeof *muse));
    assert(muse_store_state() == MUSE_OFF && muse_store_saved_state() == MUSE_PAIRED);
}

static void assert_main_preferences(const uint8_t key[32]) {
    assert(!strcmp(g_settings.name, "My Kubik") && g_settings.guide_done);
    assert(g_settings.volume == 63 && g_settings.ui_volume == 41 && g_settings.brightness == 175);
    uint8_t loaded_key[32]; size_t size = sizeof loaded_key;
    assert(nvs_get_blob(1, "devkey", loaded_key, &size) == ESP_OK && !memcmp(key, loaded_key, sizeof loaded_key));
}

static void test_main_networks_migration(bool failed_write) {
    main_networks_t old = {.magic = 0x4b574946u, .version = 1, .count = 2, .active = 1,
        .profiles = {{"first", "first-password"}, {"second", "second-password"}},
        .url = "wss://current.example/kubik/v1"};
    muse_credentials_t muse = {.magic = MUSE_STORE_MAGIC, .state = MUSE_PAIRED, .enabled = 0,
        .access_token = "saved-access", .refresh_token = "saved-refresh"};
    memcpy(muse.sdk_token, "mgst_", 5);
    memset(muse.sdk_token + 5, 'A', 43);
    const uint8_t pin[32] = {0x6b}, key[32] = {0x37};
    nvs_flash_erase();
    assert(nvs_set_blob(1, "networks", &old, sizeof old) == ESP_OK);
    assert(nvs_set_blob(1, "pin", pin, sizeof pin) == ESP_OK);
    assert(nvs_set_blob(1, "muse_v1", &muse, sizeof muse) == ESP_OK);
    assert(nvs_set_blob(1, "devkey", key, sizeof key) == ESP_OK);
    assert(nvs_set_str(1, "name", "My Kubik") == ESP_OK);
    assert(nvs_set_i32(1, "volume", 63) == ESP_OK);
    assert(nvs_set_i32(1, "ui_volume", 41) == ESP_OK);
    assert(nvs_set_i32(1, "bright", 175) == ESP_OK);
    assert(nvs_set_i32(1, "guide_done", 1) == ESP_OK);
    if (failed_write) fail_write = CONNECTION_RECORD_KEY_A;
    settings_load();
    if (failed_write) {
        assert(!connection_present() && key_present("networks") && key_present("pin") && key_present("muse_v1"));
        main_networks_t retained;
        size_t size = sizeof retained;
        assert(nvs_get_blob(1, "networks", &retained, &size) == ESP_OK && !memcmp(&old, &retained, sizeof old));
        assert_main_networks(&old, pin);
        assert_main_muse(&muse);
        assert_main_preferences(key);
    }
    fail_write = NULL;
    for (int load = 0; load < 2; load++) {
        settings_load();
        assert(connection_present() && !key_present("networks") && !key_present("pin") && !key_present("muse_v1"));
        assert_main_networks(&old, pin);
        assert_main_muse(&muse);
        assert_main_preferences(key);
    }
}

static void seed_old(const char *ssid, const char *pass, const char *url) {
    typedef struct { char ssid[33]; char password[65]; } old_wifi_t;
    typedef struct { char url[128], token[80]; } old_server_t;
    old_wifi_t wifi = {0}; old_server_t server = {0};
    snprintf(wifi.ssid, sizeof wifi.ssid, "%s", ssid);
    snprintf(wifi.password, sizeof wifi.password, "%s", pass);
    snprintf(server.url, sizeof server.url, "%s", url);
    nvs_set_blob(1, "wifi", &wifi, sizeof wifi);
    nvs_set_blob(1, "server", &server, sizeof server);
}

static void test_legacy_read_failures_and_authority(void) {
    const char *url = "wss://legacy/kubik/v1";
    nvs_flash_erase();
    seed_old("safe-net", "safe-password", url);
    fail_read = "wifi";
    settings_load();
    fail_read = NULL;
    assert(!connection_present() && key_present("wifi") && key_present("server"));
    assert(!g_settings.wifi_ssid[0] && !g_settings.server_url[0]);
    settings_load();
    assert(connection_present() && !key_present("wifi") && !key_present("server"));
    assert(!strcmp(g_settings.wifi_ssid, "safe-net") && !strcmp(g_settings.server_url, url));

    nvs_flash_erase();
    seed_old("stale-net", "stale-password", url);
    connection_networks_t malformed = {0};
    malformed.magic = 0xdeadbeefu;
    malformed.version = 1;
    nvs_set_blob(1, "networks", &malformed, sizeof malformed);
    settings_load();
    assert(!connection_present() && key_present("networks") && key_present("wifi") && key_present("server"));
    assert(!g_settings.wifi_ssid[0] && !g_settings.server_url[0]);
}

static void test_equal_revision_conflict_rejected(void) {
    nvs_flash_erase();
    settings_load();
    connection_record_t first;
    size_t size = sizeof first;
    assert(nvs_get_blob(1, CONNECTION_RECORD_KEY_A, &first, &size) == ESP_OK && size == sizeof first);
    connection_record_t second = first;
    snprintf(second.networks.url, sizeof second.networks.url, "kubik://different.local");
    assert(nvs_set_blob(1, CONNECTION_RECORD_KEY_B, &second, sizeof second) == ESP_OK);
    settings_load();
    assert(!g_settings.wifi_ssid[0] && !g_settings.server_url[0]);
    assert(settings_save_connection("blocked", "blocked-password", "kubik://blocked.local") == ESP_ERR_INVALID_STATE);
    assert(key_present(CONNECTION_RECORD_KEY_A) && key_present(CONNECTION_RECORD_KEY_B));
}

static void test_profile_limit_and_reload(void) {
    nvs_flash_erase();
    settings_load();
    char ssid[33];
    for (int i = 0; i < SETTINGS_WIFI_MAX; i++) {
        snprintf(ssid, sizeof ssid, "saved-%d", i);
        assert(settings_save_connection(ssid, "saved-password", "") == ESP_OK);
    }
    assert(g_settings.wifi_profile_count == SETTINGS_WIFI_MAX);
    assert(settings_save_connection("saved-3", "updated-password", "") == ESP_OK);
    assert(!strcmp(g_settings.wifi_profiles[3].password, "updated-password"));
    assert(settings_save_connection("saved-overflow", "saved-password", "") == ESP_ERR_NO_MEM);
    settings_load();
    assert(g_settings.wifi_profile_count == SETTINGS_WIFI_MAX);
    assert(settings_wifi_find("saved-7") >= 0 && settings_wifi_find("missing") < 0);
    assert(!strcmp(g_settings.wifi_profiles[3].password, "updated-password"));
}

void settings_test_persistence(void) {
    test_server_forms_and_pin();
    test_current_connection_migration();
    test_open_network_string_migration();
    test_main_networks_migration(false);
    test_main_networks_migration(true);
    test_legacy_read_failures_and_authority();
    test_equal_revision_conflict_rejected();
    test_profile_limit_and_reload();
}
