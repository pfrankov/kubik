#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "../main/settings.h"

typedef struct { char key[16]; unsigned char data[1200]; size_t size; } entry_t;
static entry_t stored[20], pending[20];
static const char *fail_write, *fail_erase;
static bool fail_commit, fail_erase_all;
static entry_t *records(nvs_handle_t handle) { return handle == 2 ? pending : stored; }
static int find(entry_t *list, const char *key) {
    for (int i = 0; i < 20; i++) if (!strcmp(list[i].key, key)) return i;
    return -1;
}
static bool has(const char *key) { return find(stored, key) >= 0; }

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    (void)name;
    *handle = mode == NVS_READWRITE ? 2 : 1;
    if (*handle == 2) memcpy(pending, stored, sizeof pending);
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_commit(nvs_handle_t handle) {
    if (fail_commit) return ESP_FAIL;
    if (handle == 2) memcpy(stored, pending, sizeof stored);
    return ESP_OK;
}
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { memset(stored, 0, sizeof stored); return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t handle) {
    if (fail_erase_all) return ESP_FAIL;
    memset(records(handle), 0, sizeof stored);
    return ESP_OK;
}
esp_err_t esp_read_mac(uint8_t *mac, int type) { (void)type; memcpy(mac, "ABCDEF", 6); return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    if (fail_erase && !strcmp(fail_erase, key)) return ESP_FAIL;
    entry_t *list = records(handle);
    int index = find(list, key);
    if (index < 0) return ESP_ERR_NVS_NOT_FOUND;
    memset(&list[index], 0, sizeof list[index]);
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size) {
    if (fail_write && !strcmp(fail_write, key)) return ESP_FAIL;
    entry_t *list = records(handle);
    int index = find(list, key);
    if (index < 0) for (index = 0; index < 20 && list[index].key[0]; index++);
    assert(index < 20 && size <= sizeof list[index].data);
    strcpy(list[index].key, key);
    memcpy(list[index].data, data, size);
    list[index].size = size;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size) {
    entry_t *list = records(handle);
    int index = find(list, key);
    if (index < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < list[index].size) return ESP_FAIL;
    memcpy(data, list[index].data, list[index].size);
    *size = list[index].size;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *s) { return nvs_set_blob(h, key, s, strlen(s) + 1); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *s, size_t *n) { return nvs_get_blob(h, key, s, n); }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v) { return nvs_set_blob(h, key, &v, sizeof v); }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *v) { size_t n = sizeof *v; return nvs_get_blob(h, key, v, &n); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) { return nvs_set_blob(h, key, &v, sizeof v); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v) { size_t n = sizeof *v; return nvs_get_blob(h, key, v, &n); }

typedef struct { char ssid[33], password[65]; } old_wifi_t;
typedef struct { char url[128], token[80]; } old_server_t;
typedef struct { char ssid[33], password[65], url[128]; } old_connection_t;
static void seed_old(const char *ssid, const char *pass, const char *url, const char *token) {
    old_wifi_t wifi = {0}; old_server_t server = {0};
    snprintf(wifi.ssid, sizeof wifi.ssid, "%s", ssid);
    snprintf(wifi.password, sizeof wifi.password, "%s", pass);
    snprintf(server.url, sizeof server.url, "%s", url);
    snprintf(server.token, sizeof server.token, "%s", token);
    nvs_set_blob(1, "wifi", &wifi, sizeof wifi);
    nvs_set_blob(1, "server", &server, sizeof server);
}
static void assert_connection(const char *ssid, const char *pass, const char *url) {
    settings_load();
    assert(!strcmp(g_settings.wifi_ssid, ssid));
    assert(!strcmp(g_settings.wifi_pass, pass));
    assert(!strcmp(g_settings.server_url, url));
}

static void test_old_voice_keys_dropped(void) {  // the voice choice used to live in NVS: a save clears it
    nvs_flash_erase();
    assert(nvs_set_str(1, "vengine", "live") == ESP_OK);
    assert(nvs_set_str(1, "vname", "alloy") == ESP_OK);
    settings_load();
    assert(has("vengine") && has("vname"));
    settings_save();
    assert(!has("vengine") && !has("vname"));
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
    assert_connection("home", "home-password", "");
    assert(!g_settings.server_pinned);
    uint8_t pin[SETTINGS_PIN_BYTES];
    memset(pin, 0xa5, sizeof pin);
    assert(settings_save_pin(pin) == ESP_OK && g_settings.server_pinned);
    settings_load();
    assert(g_settings.server_pinned && !memcmp(g_settings.server_pin, pin, sizeof pin));
    // A failed save keeps both the old record and its pin.
    fail_write = "networks";
    assert(settings_save_connection("home", "home-password", "kubik://other.local") == ESP_FAIL);
    fail_write = NULL;
    settings_load();
    assert(g_settings.server_pinned && !strcmp(g_settings.server_url, ""));
    // Any explicit save forgets the pin, even of the same server (the user's way out of "key changed").
    assert(settings_save_connection("home", "home-password", "") == ESP_OK);
    assert(!g_settings.server_pinned && !has("pin"));
    assert(settings_save_pin(pin) == ESP_OK);
    assert(settings_save_connection("home", "home-password", "kubik://other.local:18790") == ESP_OK);
    settings_load();
    assert(!g_settings.server_pinned && !strcmp(g_settings.server_url, "kubik://other.local:18790"));
    // A damaged pin record is no pin at all.
    nvs_set_blob(1, "pin", pin, 16);
    settings_load();
    assert(!g_settings.server_pinned);
}

static void test_current_connection_migration(void) {
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
    assert(has("networks") && !has("connection"));
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

static void test_reset_failures(void) {
    fail_erase_all = true;
    assert(settings_factory_reset() == ESP_FAIL);
    fail_erase_all = false;
    fail_commit = true;
    assert(settings_factory_reset() == ESP_FAIL);
    fail_commit = false;
    assert(has("devkey") && has("networks") && g_settings.volume == 15);
}

static void test_factory_reset(void) {
    nvs_flash_erase();
    const uint8_t key[32] = {7};
    uint8_t pin[SETTINGS_PIN_BYTES] = {0xa5};
    nvs_set_blob(1, "devkey", key, sizeof key);
    assert(settings_save_connection("Odin", "odin-password", "kubik://box.local") == ESP_OK);
    assert(settings_save_pin(pin) == ESP_OK);
    settings_load();
    g_settings.volume = 15; g_settings.brightness = 40; g_settings.greeted = true;
    assert(settings_complete_guide() == ESP_OK);
    strcpy(g_settings.name, "Mine");
    nvs_set_str(1, "vengine", "live");
    nvs_set_u8(1, "setup", 1);
    // A failed erase leaves the record and the in-memory settings as they were.
    test_reset_failures();
    assert(!strcmp(g_settings.wifi_ssid, "Odin") && g_settings.volume == 15);
    assert(settings_factory_reset() == ESP_OK);
    assert(!g_settings.wifi_ssid[0] && !g_settings.wifi_pass[0] && !g_settings.server_url[0] && !g_settings.server_pinned);
    assert(g_settings.volume == 70 && g_settings.brightness == 200 &&
           !g_settings.greeted && !g_settings.guide_done && !g_settings.event_overlay && !strcmp(g_settings.name, "Kubik"));
    const char *gone[] = {"networks", "connection", "pin", "devkey", "name", "character", "volume", "bright", "greeted", "guide_done", "event_overlay", "setup", "vengine"};
    for (size_t i = 0; i < sizeof gone / sizeof gone[0]; i++) assert(!has(gone[i]));
    assert(!has("devkey"));  // factory reset also clears the device identity key
    settings_load();
    assert(!g_settings.wifi_ssid[0] && g_settings.volume == 70 && !g_settings.server_pinned);
    assert(settings_factory_reset() == ESP_OK);  // already clean: still fine
}

static void test_guide_storage(void) {
    settings_load(); assert(!g_settings.guide_done);
    assert(settings_save_connection("guide-network", "guide-password", "kubik://guide.local") == ESP_OK);
    settings_t before = g_settings;
    fail_write = "guide_done";
    assert(settings_complete_guide() == ESP_FAIL && !g_settings.guide_done);
    fail_write = NULL; fail_commit = true;
    assert(settings_complete_guide() == ESP_FAIL && !g_settings.guide_done);
    fail_commit = false; settings_load(); assert(!g_settings.guide_done);
    assert(settings_complete_guide() == ESP_OK);
    before.guide_done = true; assert(!memcmp(&before, &g_settings, sizeof before));
    settings_save(); settings_load(); assert(g_settings.guide_done);
    fail_commit = true;
    assert(settings_complete_guide() == ESP_OK); // already seen: no extra write
    fail_commit = false;
    nvs_flash_erase();
}

static void test_split_volume(void) {
    memset(stored, 0, sizeof stored);
    nvs_set_i32(1, "volume", 35); settings_load();
    assert(g_settings.volume == 35 && g_settings.ui_volume == 35); // migrate the old shared setting
    assert(!g_settings.event_overlay);
    g_settings.event_overlay = true;
    g_settings.ui_volume = 0; g_settings.volume = 80; settings_save(); settings_load();
    assert(g_settings.volume == 80 && g_settings.ui_volume == 0 && g_settings.event_overlay);
    assert(settings_factory_reset() == ESP_OK);
    assert(g_settings.volume == 70 && g_settings.ui_volume == 70);
    nvs_set_i32(1, "volume", 999); nvs_set_i32(1, "ui_volume", -1); settings_load();
    assert(g_settings.volume == 70 && g_settings.ui_volume == 70);
}
static void english_default_name(void) {
    assert(nvs_set_str(1, "name", "Кубик") == ESP_OK);
    settings_load(); assert(!strcmp(g_settings.name, "Kubik"));
    char saved[32]; size_t size=sizeof saved;
    assert(nvs_get_str(1,"name",saved,&size)==ESP_OK && !strcmp(saved,"Kubik"));
    assert(nvs_set_str(1,"name","Мой робот")==ESP_OK);
    settings_load(); assert(!strcmp(g_settings.name,"Мой робот"));
    assert(nvs_set_str(1,"name","Кубик")==ESP_OK); fail_commit=true;
    settings_load(); assert(!strcmp(g_settings.name,"Kubik"));
    size=sizeof saved; assert(nvs_get_str(1,"name",saved,&size)==ESP_OK && !strcmp(saved,"Кубик"));
    fail_commit=false; settings_load(); size=sizeof saved;
    assert(nvs_get_str(1,"name",saved,&size)==ESP_OK && !strcmp(saved,"Kubik"));
    nvs_erase_key(1,"name");
}
int main(void) {
    english_default_name();
    const char *old_url = "wss://old-host/kubik/v1", *new_url = "wss://example.com/kubik/v1";
    test_factory_reset();
    test_guide_storage();
    nvs_set_i32(1, "character", 0); // Retired preferences do not select a firmware character.
    settings_load();
    settings_t loaded = g_settings;
    nvs_set_i32(1, "character", 1);
    settings_load();
    assert(!memcmp(&loaded, &g_settings, sizeof loaded));
    nvs_flash_erase();
    seed_old("old-network", "old-password", old_url, "retired-secret");
    fail_write = "networks";
    assert_connection("old-network", "old-password", old_url);
    assert(has("server") && has("wifi") && !has("networks"));
    fail_write = NULL; fail_commit = true;
    assert_connection("old-network", "old-password", old_url);
    assert(has("server") && !has("networks"));
    fail_commit = false;
    assert_connection("old-network", "old-password", old_url);
    assert(has("networks") && !has("connection") && !has("server") && !has("wifi") && !has("token"));

    assert(settings_save_connection("new-network", "new-password", new_url) == ESP_OK);
    assert_connection("new-network", "new-password", new_url);
    assert(g_settings.wifi_profile_count == 2);
    assert(settings_save_connection("old-network", "", new_url) == ESP_OK);
    assert(g_settings.wifi_profile_count == 2 && !strcmp(g_settings.wifi_profiles[0].password, "old-password"));
    assert(settings_save_connection("old-network", "updated-password", new_url) == ESP_OK);
    assert(!strcmp(g_settings.wifi_profiles[0].password, "updated-password"));
    fail_write = "networks";
    assert(settings_save_connection("broken", "new-password", old_url) == ESP_FAIL);
    fail_write = NULL;
    assert_connection("old-network", "updated-password", new_url);
    fail_commit = true;
    assert(settings_save_connection("broken", "new-password", old_url) == ESP_FAIL);
    fail_commit = false;
    assert_connection("old-network", "updated-password", new_url);

    const char *invalid[] = {"ws://10.0.0.2:18790/kubik/v1", "https://example.com/x", "ws://", "ws:///x", "ws://host", "ws://user@host/x",
        "ws://host/x?secret=y", "ws://host:0/x", "ws://host:65536/x", "ws://bad..host/x", "ws://[::::]/x"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
        assert(settings_save_connection("new-network", "new-password", invalid[i]) == ESP_ERR_INVALID_ARG);
    assert(settings_save_connection("new-network", "short", new_url) == ESP_ERR_INVALID_ARG);
    assert(settings_save_connection(g_settings.wifi_ssid, g_settings.wifi_pass, g_settings.server_url) == ESP_OK);
    strcpy(g_settings.wifi_ssid, "unsaved"); settings_save();
    assert_connection("old-network", "updated-password", new_url);

    assert(!settings_take_provisioning());
    nvs_set_u8(1, "setup", 1);
    fail_erase = "setup"; assert(!settings_take_provisioning());
    fail_erase = NULL; assert(settings_take_provisioning() && !settings_take_provisioning());

    // A damaged current list never falls back to old, possibly secret-bearing records.
    seed_old("stale", "stale-pass", old_url, "stale-token");
    unsigned char corrupt[1200];
    memset(corrupt, 'x', sizeof corrupt);
    nvs_set_blob(1, "networks", corrupt, sizeof corrupt);
    assert_connection("", "", "");
    test_split_volume();
    test_old_voice_keys_dropped();
    test_server_forms_and_pin();
    test_current_connection_migration();
    test_profile_limit_and_reload();
    puts("settings: atomic saved Wi-Fi list and URL, exact-SSID upsert, legacy migration, old voice keys dropped, failed write/commit, factory reset clearing the device key, validation, no stale revival, "
         "LAN/kubik:// server forms and pin clearing passed");
}
