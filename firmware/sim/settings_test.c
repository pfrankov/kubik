#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "../main/settings.h"
#include "../main/muse_store.h"
#include "../main/connection_record.h"
#include "../main/connection_store.h"

extern bool app_nvs_test_lock_is_held(void);
static pthread_mutex_t apply_race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t apply_race_cond = PTHREAD_COND_INITIALIZER;
static bool block_first_apply, first_apply_entered, release_first_apply;
static bool second_save_started, second_save_done;
static esp_err_t first_save_result, second_save_result;

void settings_test_apply_hook(const connection_record_t *record) {
    if (!block_first_apply || !record->networks.count ||
        strcmp(record->networks.profiles[record->networks.active].ssid, "race-first")) return;
    pthread_mutex_lock(&apply_race_mutex);
    first_apply_entered = true;
    pthread_cond_broadcast(&apply_race_cond);
    while (!release_first_apply) pthread_cond_wait(&apply_race_cond, &apply_race_mutex);
    pthread_mutex_unlock(&apply_race_mutex);
}

typedef struct { char key[16]; unsigned char data[6000]; size_t size; } entry_t;
static entry_t stored[24];
const char *fail_write, *fail_read;
static const char *fail_erase;
static bool fail_after_write, fail_erase_all;
static bool fail_connection_allocation;
void *settings_test_calloc(size_t count, size_t size) {
    if (fail_connection_allocation) return NULL;
    void *memory = malloc(count * size);
    if (memory) memset(memory, 0, count * size);
    return memory;
}
static entry_t *records(nvs_handle_t handle) { (void)handle; return stored; }
static int find(entry_t *list, const char *key) {
    for (int i = 0; i < 24; i++) if (!strcmp(list[i].key, key)) return i;
    return -1;
}
static bool has(const char *key) { return find(stored, key) >= 0; }
static bool empty_store(void) {
    for (int i = 0; i < 24; i++) if (stored[i].key[0]) return false;
    return true;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    (void)name;
    if (mode == NVS_READONLY && empty_store()) return ESP_ERR_NVS_NOT_FOUND;
    *handle = mode == NVS_READWRITE ? 2 : 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_deinit(void) { return ESP_OK; }
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
    bool fail = fail_write && !strcmp(fail_write, key);
    if (fail && !fail_after_write) return ESP_FAIL;
    entry_t *list = records(handle);
    int index = find(list, key);
    if (index < 0) for (index = 0; index < 24 && list[index].key[0]; index++);
    assert(index < 24 && size <= sizeof list[index].data);
    strcpy(list[index].key, key);
    memcpy(list[index].data, data, size);
    list[index].size = size;
    return fail ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size) {
    if (fail_read && !strcmp(fail_read, key)) return ESP_FAIL;
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
const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock"; }
void mbedtls_platform_zeroize(void *data, size_t size) { volatile unsigned char *p = data; while (size--) *p++ = 0; }
int mbedtls_sha256(const unsigned char *input, size_t length, unsigned char output[32], int is224) {
    (void)is224;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < length; i++) { hash ^= input[i]; hash *= 1099511628211ull; }
    for (size_t i = 0; i < 32; i++) { hash ^= i + 0x9e3779b97f4a7c15ull; hash *= 1099511628211ull; output[i] = hash >> ((i & 7) * 8); }
    return 0;
}

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

static const char *record_key(unsigned slot) {
    return slot ? CONNECTION_RECORD_KEY_B : CONNECTION_RECORD_KEY_A;
}
static int64_t record_revision(unsigned slot) {
    connection_record_t record;
    size_t size = sizeof record;
    esp_err_t err = nvs_get_blob(1, record_key(slot), &record, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) return -1;
    assert(err == ESP_OK && size == sizeof record && record.magic == CONNECTION_RECORD_MAGIC &&
           record.version == CONNECTION_RECORD_VERSION && record.revision);
    return (int64_t)record.revision;
}
static const char *next_record_key(void) {
    int64_t a = record_revision(0), b = record_revision(1);
    if (a < 0) return CONNECTION_RECORD_KEY_A;
    if (b < 0) return CONNECTION_RECORD_KEY_B;
    return a >= b ? CONNECTION_RECORD_KEY_B : CONNECTION_RECORD_KEY_A;
}
static const char *newest_record_key(void) {
    int64_t a = record_revision(0), b = record_revision(1);
    assert(a >= 0 || b >= 0);
    if (a < 0) return CONNECTION_RECORD_KEY_B;
    if (b < 0) return CONNECTION_RECORD_KEY_A;
    return a >= b ? CONNECTION_RECORD_KEY_A : CONNECTION_RECORD_KEY_B;
}
static bool has_connection_record(void) {
    return has(CONNECTION_RECORD_KEY_A) || has(CONNECTION_RECORD_KEY_B);
}

static void *save_race_first(void *unused) {
    (void)unused;
    first_save_result = settings_save_connection("race-first", "first-password", "kubik://first.local");
    return NULL;
}
static void *save_race_second(void *unused) {
    (void)unused;
    pthread_mutex_lock(&apply_race_mutex);
    second_save_started = true;
    pthread_cond_broadcast(&apply_race_cond);
    pthread_mutex_unlock(&apply_race_mutex);
    second_save_result = settings_save_connection("race-second", "second-password", "kubik://second.local");
    pthread_mutex_lock(&apply_race_mutex);
    second_save_done = true;
    pthread_cond_broadcast(&apply_race_cond);
    pthread_mutex_unlock(&apply_race_mutex);
    return NULL;
}
static void test_apply_serializes_with_storage(void) {
    nvs_flash_erase();
    settings_load();
    pthread_mutex_lock(&apply_race_mutex);
    block_first_apply = true;
    first_apply_entered = release_first_apply = second_save_started = second_save_done = false;
    first_save_result = second_save_result = ESP_FAIL;
    pthread_mutex_unlock(&apply_race_mutex);

    pthread_t first, second;
    assert(pthread_create(&first, NULL, save_race_first, NULL) == 0);
    pthread_mutex_lock(&apply_race_mutex);
    while (!first_apply_entered) pthread_cond_wait(&apply_race_cond, &apply_race_mutex);
    pthread_mutex_unlock(&apply_race_mutex);
    assert(pthread_create(&second, NULL, save_race_second, NULL) == 0);
    pthread_mutex_lock(&apply_race_mutex);
    while (!second_save_started) pthread_cond_wait(&apply_race_cond, &apply_race_mutex);
    pthread_mutex_unlock(&apply_race_mutex);

    bool connection_lock_held_during_apply = app_nvs_test_lock_is_held();
    pthread_mutex_lock(&apply_race_mutex);
    release_first_apply = true;
    pthread_cond_broadcast(&apply_race_cond);
    pthread_mutex_unlock(&apply_race_mutex);
    assert(pthread_join(first, NULL) == 0 && pthread_join(second, NULL) == 0);
    pthread_mutex_lock(&apply_race_mutex);
    block_first_apply = false;
    pthread_mutex_unlock(&apply_race_mutex);

    assert(connection_lock_held_during_apply);
    assert(first_save_result == ESP_OK && second_save_result == ESP_OK && second_save_done);
    assert(!strcmp(g_settings.wifi_ssid, "race-second") &&
           !strcmp(g_settings.server_url, "kubik://second.local"));
    settings_load();
    assert(!strcmp(g_settings.wifi_ssid, "race-second") &&
           !strcmp(g_settings.server_url, "kubik://second.local"));
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

static void test_allocator_failure_and_revision_guards(void) {
    nvs_flash_erase();
    settings_load();
    assert(settings_save_connection("kept-net", "kept-password", "kubik://kept.local") == ESP_OK);
    const char *current_key = newest_record_key();
    connection_record_t before;
    size_t size = sizeof before;
    assert(nvs_get_blob(1, current_key, &before, &size) == ESP_OK && size == sizeof before);
    fail_connection_allocation = true;
    assert(settings_save_connection("lost-net", "lost-password", "kubik://lost.local") == ESP_ERR_NO_MEM);
    fail_connection_allocation = false;
    connection_record_t after;
    size = sizeof after;
    assert(nvs_get_blob(1, current_key, &after, &size) == ESP_OK && size == sizeof after);
    assert(!memcmp(&before, &after, sizeof before));

    before.revision = UINT64_MAX;
    assert(nvs_set_blob(1, current_key, &before, sizeof before) == ESP_OK);
    assert(settings_save_connection("overflow-net", "overflow-password", "kubik://overflow.local") == ESP_ERR_INVALID_STATE);
    settings_load();
    assert(!strcmp(g_settings.wifi_ssid, "kept-net"));
}

static void test_setup_connection_and_agent_share_record(void) {
    nvs_flash_erase();
    settings_load();
    assert(settings_save_connection("previous-net", "previous-pass", "kubik://previous.local") == ESP_OK);
    uint8_t pin[SETTINGS_PIN_BYTES] = {0x5a};
    assert(settings_save_pin(pin) == ESP_OK);
    char token[49] = "mgst_";
    memset(token + 5, 'A', 43); token[48] = 0;
    assert(muse_store_begin(token) == ESP_OK);
    assert(muse_store_state() == MUSE_PAIRING);

    // This models SET-06: the connection and chosen agent now share one write.
    fail_write = next_record_key();
    assert(settings_save_setup("candidate-net", "candidate-pass", "kubik://candidate.local", false, NULL) == ESP_FAIL);
    fail_write = NULL;
    settings_load();
    assert(!strcmp(g_settings.wifi_ssid, "previous-net"));
    assert(!strcmp(g_settings.server_url, "kubik://previous.local"));
    assert(g_settings.server_pinned && !memcmp(g_settings.server_pin, pin, sizeof pin));
    assert(muse_store_state() == MUSE_PAIRING);

    assert(settings_save_setup("candidate-net", "candidate-pass", "kubik://candidate.local", false, NULL) == ESP_OK);
    assert(!strcmp(g_settings.wifi_ssid, "candidate-net") && !g_settings.server_pinned);
    assert(muse_store_state() == MUSE_OFF && muse_store_saved_state() == MUSE_PAIRING);
}

static void test_reset_failures(void) {
    fail_erase_all = true;
    assert(settings_factory_reset() == ESP_FAIL);
    fail_erase_all = false;
    assert(has("devkey") && has_connection_record() && g_settings.volume == 15);
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
    const char *gone[] = {CONNECTION_RECORD_KEY_A, CONNECTION_RECORD_KEY_B, "networks", "connection", "pin", "devkey", "name", "character", "volume", "bright", "greeted", "guide_done", "event_overlay", "setup", "vengine"};
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
    fail_write = NULL; settings_load(); assert(!g_settings.guide_done);
    assert(settings_complete_guide() == ESP_OK);
    before.guide_done = true; assert(!memcmp(&before, &g_settings, sizeof before));
    settings_save(); settings_load(); assert(g_settings.guide_done);
    assert(settings_complete_guide() == ESP_OK); // already seen: no extra write
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
    assert(nvs_set_str(1,"name","Кубик")==ESP_OK); fail_write="name";
    settings_load(); assert(!strcmp(g_settings.name,"Kubik"));
    size=sizeof saved; assert(nvs_get_str(1,"name",saved,&size)==ESP_OK && !strcmp(saved,"Кубик"));
    fail_write=NULL; settings_load(); size=sizeof saved;
    assert(nvs_get_str(1,"name",saved,&size)==ESP_OK && !strcmp(saved,"Kubik"));
    nvs_erase_key(1,"name");
}
void settings_test_connection_runtime(void);
void settings_test_persistence(void);

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
    fail_write = CONNECTION_RECORD_KEY_A;
    assert_connection("old-network", "old-password", old_url);
    assert(has("server") && has("wifi") && !has_connection_record());
    fail_write = NULL;
    assert_connection("old-network", "old-password", old_url);
    assert(has_connection_record() && !has("connection") && !has("server") && !has("wifi") && !has("token"));

    assert(settings_save_connection("new-network", "new-password", new_url) == ESP_OK);
    assert_connection("new-network", "new-password", new_url);
    assert(g_settings.wifi_profile_count == 2);
    assert(settings_save_connection("old-network", "", new_url) == ESP_OK);
    assert(g_settings.wifi_profile_count == 2 && !strcmp(g_settings.wifi_profiles[0].password, "old-password"));
    assert(settings_save_connection("old-network", "updated-password", new_url) == ESP_OK);
    assert(!strcmp(g_settings.wifi_profiles[0].password, "updated-password"));
    fail_write = next_record_key();
    assert(settings_save_connection("broken", "new-password", old_url) == ESP_FAIL);
    fail_write = NULL;
    assert_connection("old-network", "updated-password", new_url);
    // A write that reports an error after immediate mutation is resolved by
    // reinitializing NVS and confirms the new complete record.
    fail_write = next_record_key(); fail_after_write = true;
    assert(settings_save_connection("written-before-error", "new-password", old_url) == ESP_OK);
    fail_write = NULL; fail_after_write = false;
    assert_connection("written-before-error", "new-password", old_url);
    assert(g_settings.wifi_profile_count == 3);

    const char *invalid[] = {"ws://10.0.0.2:18790/kubik/v1", "https://example.com/x", "ws://", "ws:///x", "ws://host", "ws://user@host/x",
        "ws://host/x?secret=y", "ws://host:0/x", "ws://host:65536/x", "ws://bad..host/x", "ws://[::::]/x"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
        assert(settings_save_connection("new-network", "new-password", invalid[i]) == ESP_ERR_INVALID_ARG);
    assert(settings_save_connection("new-network", "short", new_url) == ESP_ERR_INVALID_ARG);
    assert(settings_save_connection(g_settings.wifi_ssid, g_settings.wifi_pass, g_settings.server_url) == ESP_OK);
    strcpy(g_settings.wifi_ssid, "unsaved"); settings_save();
    assert_connection("written-before-error", "new-password", old_url);

    assert(!settings_take_provisioning());
    nvs_set_u8(1, "setup", 1);
    fail_erase = "setup"; assert(!settings_take_provisioning());
    fail_erase = NULL; assert(settings_take_provisioning() && !settings_take_provisioning());

    // A damaged slot never falls back to old, possibly secret-bearing records.
    nvs_flash_erase();
    seed_old("stale", "stale-pass", old_url, "stale-token");
    unsigned char corrupt[1200];
    memset(corrupt, 'x', sizeof corrupt);
    nvs_set_blob(1, CONNECTION_RECORD_KEY_A, corrupt, sizeof corrupt);
    assert_connection("", "", "");
    test_split_volume();
    test_old_voice_keys_dropped();
    test_allocator_failure_and_revision_guards();
    test_apply_serializes_with_storage();
    test_setup_connection_and_agent_share_record();
    settings_test_persistence();
    settings_test_connection_runtime();
    puts("settings: A/B record selection, migration read failures, revision guards, serialized in-memory apply, "
         "exact-SSID upsert, preferences, factory reset, URL validation and pin clearing passed");
}
