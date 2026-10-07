#include "settings.h"
#include "app_nvs.h"
#include "connection_store.h"
#include "muse_store.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

static const char *TAG = "settings";

settings_t g_settings;
char g_device_id[20];

static void get_str(nvs_handle_t h, const char *key, char *out, size_t cap, const char *def) {
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) snprintf(out, cap, "%s", def);
}

static int get_int(nvs_handle_t h, const char *key, int def) {
    int32_t v;
    return nvs_get_i32(h, key, &v) == ESP_OK ? (int)v : def;
}

bool settings_server_valid(const char *url) { return connection_store_server_valid(url); }

int settings_wifi_find(const char *ssid) {
    if (!ssid || !ssid[0]) return -1;
    for (int i = 0; i < g_settings.wifi_profile_count; i++)
        if (!strcmp(g_settings.wifi_profiles[i].ssid, ssid)) return i;
    return -1;
}

static void set_active_profile(settings_t *s, int index) {
    memset(s->wifi_ssid, 0, sizeof s->wifi_ssid);
    memset(s->wifi_pass, 0, sizeof s->wifi_pass);
    if (index < 0 || index >= s->wifi_profile_count) return;
    memcpy(s->wifi_ssid, s->wifi_profiles[index].ssid, sizeof s->wifi_ssid);
    memcpy(s->wifi_pass, s->wifi_profiles[index].password, sizeof s->wifi_pass);
}

static void set_defaults(settings_t *s) {
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "Kubik");
    s->volume = 70;
    s->ui_volume = 70;
    s->brightness = 200;
}

static int load_volume(nvs_handle_t h, const char *key, int fallback) {
    int value = get_int(h, key, fallback);
    return value >= 0 && value <= 100 ? value : 70;
}

static void migrate_default_name(settings_t *s) {
    if (strcmp(s->name, "Кубик")) return;
    snprintf(s->name, sizeof s->name, "Kubik");
    esp_err_t err = app_nvs_lock();
    if (err != ESP_OK) return;
    nvs_handle_t h;
    if (!app_nvs_ready_locked() || nvs_open("kubik", NVS_READWRITE, &h) != ESP_OK) {
        app_nvs_unlock();
        return;
    }
    err = nvs_set_str(h, "name", s->name);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not save English default name");
        if (app_nvs_recover_locked() != ESP_OK) app_nvs_mark_unhealthy_locked();
    }
    app_nvs_unlock();
}

void settings_apply_connection_record(const connection_record_t *record) {
    if (!record || record->magic != CONNECTION_RECORD_MAGIC || record->version != CONNECTION_RECORD_VERSION ||
        !connection_store_networks_valid(&record->networks)) return;
#ifdef APP_NVS_TEST_HOOKS
    extern void settings_test_apply_hook(const connection_record_t *record);
    settings_test_apply_hook(record);
#endif
    settings_t *s = &g_settings;
    s->wifi_profile_count = record->networks.count;
    memcpy(s->wifi_profiles, record->networks.profiles, sizeof s->wifi_profiles);
    memcpy(s->server_url, record->networks.url, sizeof s->server_url);
    set_active_profile(s, record->networks.active);
    settings_apply_connection_pin(record);
}

void settings_apply_connection_pin(const connection_record_t *record) {
    if (!record || record->magic != CONNECTION_RECORD_MAGIC || record->version != CONNECTION_RECORD_VERSION)
        return;
    settings_t *s = &g_settings;
    if (record->flags & CONNECTION_RECORD_PIN_SET) {
        memcpy(s->server_pin, record->server_pin, sizeof s->server_pin);
        s->server_pinned = true;
    } else {
        memset(s->server_pin, 0, sizeof s->server_pin);
        s->server_pinned = false;
    }
}

void settings_load(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_device_id, sizeof(g_device_id), "kubik-%02x%02x%02x", mac[3], mac[4], mac[5]);
    settings_t *s = &g_settings;
    set_defaults(s);
    muse_store_publish_state(NULL);

    esp_err_t err = app_nvs_lock();
    if (err == ESP_OK) {
        err = app_nvs_init_locked();
        app_nvs_unlock();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (%s); saved connection data was not erased", esp_err_to_name(err));
        return;
    }

    connection_record_t *record = calloc(1, sizeof *record);
    if (record) {
        bool durable = false;
        err = connection_store_load(record, &durable);
        if (err == ESP_OK) {
            settings_apply_connection_record(record);
            if (!durable) ESP_LOGW(TAG, "using legacy connection data; migration could not be saved");
        } else {
            ESP_LOGW(TAG, "saved connection data unavailable (%s)", esp_err_to_name(err));
        }
        muse_store_wipe(record, sizeof *record);
        free(record);
    } else {
        ESP_LOGW(TAG, "not enough memory to load saved connection data");
    }

    err = app_nvs_lock();
    if (err != ESP_OK) return;
    if (app_nvs_ready_locked()) {
        nvs_handle_t h;
        if (nvs_open("kubik", NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "name", s->name, sizeof(s->name), "Kubik");
        s->volume = load_volume(h, "volume", 70);
        s->ui_volume = load_volume(h, "ui_volume", s->volume);
        s->brightness = get_int(h, "bright", 200);
        s->greeted = get_int(h, "greeted", 0);
        s->event_overlay = get_int(h, "event_overlay", 0) == 1;
        s->guide_done = get_int(h, "guide_done", 0) == 1;
        nvs_close(h);
        }
    }
    app_nvs_unlock();
    migrate_default_name(s);
    ESP_LOGI(TAG, "device %s, Wi-Fi profiles %u, server %s", g_device_id, s->wifi_profile_count,
             s->server_url[0] ? "set" : "unset");
}

static esp_err_t save_preferences(nvs_handle_t h, const settings_t *s) {
    // General preferences never rewrite connection credentials.
    esp_err_t err = nvs_set_str(h, "name", s->name);
    if (err == ESP_OK) err = nvs_set_i32(h, "volume", s->volume);
    if (err == ESP_OK) err = nvs_set_i32(h, "ui_volume", s->ui_volume);
    if (err == ESP_OK) err = nvs_set_i32(h, "bright", s->brightness);
    if (err == ESP_OK) err = nvs_set_i32(h, "greeted", s->greeted);
    if (err == ESP_OK) err = nvs_set_i32(h, "event_overlay", s->event_overlay);
    const char *const retired[] = {"character", "tiltgain", "tiltinv", "vengine", "vname"};
    for (size_t i = 0; err == ESP_OK && i < sizeof retired / sizeof retired[0]; i++) {
        err = nvs_erase_key(h, retired[i]);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    return err;
}

void settings_save(void) {
    esp_err_t err = app_nvs_lock();
    if (err != ESP_OK) return;
    if (!app_nvs_ready_locked()) { app_nvs_unlock(); return; }
    nvs_handle_t h;
    err = nvs_open("kubik", NVS_READWRITE, &h);
    if (err != ESP_OK) { app_nvs_unlock(); return; }
    err = save_preferences(h, &g_settings);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not save preferences (%s)", esp_err_to_name(err));
        if (app_nvs_recover_locked() != ESP_OK) app_nvs_mark_unhealthy_locked();
    }
    app_nvs_unlock();
}

esp_err_t settings_complete_guide(void) {
    if (g_settings.guide_done) return ESP_OK;
    esp_err_t err = app_nvs_lock();
    if (err != ESP_OK) return err;
    if (!app_nvs_ready_locked()) { app_nvs_unlock(); return ESP_ERR_INVALID_STATE; }
    nvs_handle_t h;
    err = nvs_open("kubik", NVS_READWRITE, &h);
    if (err != ESP_OK) { app_nvs_unlock(); return err; }
    err = nvs_set_i32(h, "guide_done", 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK && app_nvs_recover_locked() != ESP_OK) app_nvs_mark_unhealthy_locked();
    app_nvs_unlock();
    if (err == ESP_OK) g_settings.guide_done = true;
    return err;
}

bool settings_take_provisioning(void) {
    if (app_nvs_lock() != ESP_OK) return false;
    if (!app_nvs_ready_locked()) { app_nvs_unlock(); return false; }
    nvs_handle_t h;
    esp_err_t err = nvs_open("kubik", NVS_READWRITE, &h);
    if (err != ESP_OK) { app_nvs_unlock(); return false; }
    uint8_t requested = 0;
    err = nvs_get_u8(h, "setup", &requested);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (requested) {
        if (err == ESP_OK) err = nvs_erase_key(h, "setup");
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK && app_nvs_recover_locked() != ESP_OK) app_nvs_mark_unhealthy_locked();
    app_nvs_unlock();
    // Never enter setup if the one-shot flag could not be consumed.
    return requested && err == ESP_OK;
}

esp_err_t settings_save_connection(const char *ssid, const char *password, const char *url) {
    connection_record_t *record = calloc(1, sizeof *record);
    if (!record) return ESP_ERR_NO_MEM;
    esp_err_t err = connection_store_save_network(record, ssid, password, url);
    muse_store_wipe(record, sizeof *record);
    free(record);
    return err;
}

esp_err_t settings_save_setup(const char *ssid, const char *password, const char *url,
                             bool muse_selected, const char *sdk_token) {
    connection_record_t *record = calloc(1, sizeof *record);
    if (!record) return ESP_ERR_NO_MEM;
    esp_err_t err = connection_store_save_setup(record, ssid, password, url, muse_selected, sdk_token);
    muse_store_wipe(record, sizeof *record);
    free(record);
    return err;
}

esp_err_t settings_save_pin(const uint8_t pin[SETTINGS_PIN_BYTES]) {
    connection_record_t *record = calloc(1, sizeof *record);
    if (!record) return ESP_ERR_NO_MEM;
    esp_err_t err = connection_store_save_pin(record, pin);
    muse_store_wipe(record, sizeof *record);
    free(record);
    return err;
}

esp_err_t settings_factory_reset(void) {
    esp_err_t err = app_nvs_lock();
    if (err != ESP_OK) return err;
    if (!app_nvs_ready_locked()) { app_nvs_unlock(); return ESP_ERR_INVALID_STATE; }
    nvs_handle_t h;
    err = nvs_open("kubik", NVS_READWRITE, &h);
    if (err != ESP_OK) { app_nvs_unlock(); return err; }
    err = nvs_erase_all(h);
    bool erased = err == ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK && app_nvs_recover_locked() != ESP_OK) app_nvs_mark_unhealthy_locked();
    if (erased) muse_store_publish_state(NULL);
    app_nvs_unlock();
    if (err == ESP_OK) set_defaults(&g_settings);
    return err;
}
