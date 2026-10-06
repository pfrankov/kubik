#include "settings.h"

#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#include "lwip/inet.h"
#else
#include <arpa/inet.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

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

// Older firmware stored these as separate records. They are read only for migration.
typedef struct { char ssid[33]; char password[65]; } wifi_settings_t;
static void add_migrated_profile(settings_t *s) {
    if (!s->wifi_ssid[0]) return;
    s->wifi_profile_count = 1;
    memcpy(s->wifi_profiles[0].ssid, s->wifi_ssid, sizeof s->wifi_ssid);
    memcpy(s->wifi_profiles[0].password, s->wifi_pass, sizeof s->wifi_pass);
}

static void load_wifi(nvs_handle_t h, settings_t *s) {
    wifi_settings_t pair;
    size_t size = sizeof(pair);
    esp_err_t err = nvs_get_blob(h, "wifi", &pair, &size);
    if (err == ESP_OK && size == sizeof(pair) &&
        memchr(pair.ssid, 0, sizeof(pair.ssid)) && memchr(pair.password, 0, sizeof(pair.password))) {
        memcpy(s->wifi_ssid, pair.ssid, sizeof(pair.ssid));
        memcpy(s->wifi_pass, pair.password, sizeof(pair.password));
        add_migrated_profile(s);
        return;
    }
    // Only an absent blob is a legacy installation. Corrupt data is not revived.
    if (err != ESP_ERR_NVS_NOT_FOUND) return;
    get_str(h, "ssid", s->wifi_ssid, sizeof(s->wifi_ssid), "");
    get_str(h, "pass", s->wifi_pass, sizeof(s->wifi_pass), "");
    add_migrated_profile(s);
}

// The server blob keeps its old layout; the token half (shared secrets, before
// device keys) is always written empty, which also wipes an old one.
typedef struct { char url[128]; char token[80]; } server_settings_t;
static bool valid_url_text(const char *url) {
    if (!url || strlen(url) >= 128) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p <= 32 || *p >= 127 || *p == '@' || *p == '?' || *p == '#' || *p == '\\') return false;
    return true;
}

static bool valid_ipv6_host(const char *host, const char *path, const char **port) {
    const char *end = strchr(host, ']');
    if (!end || end >= path || end == host + 1) return false;
    char literal[INET6_ADDRSTRLEN];
    size_t length = (size_t)(end - host - 1);
    struct in6_addr addr;
    if (length >= sizeof(literal)) return false;
    memcpy(literal, host + 1, length);
    literal[length] = 0;
    if (inet_pton(AF_INET6, literal, &addr) != 1) return false;
    if (end + 1 != path) {
        if (end[1] != ':') return false;
        *port = end + 2;
    }
    return true;
}

static bool valid_dns_character(const char *position, const char *host) {
    if (!isalnum((unsigned char)*position) && *position != '.' && *position != '-') return false;
    return *position != '.' ||
           (position > host && position[-1] != '.' && position[-1] != '-' && position[1] != '-');
}

static bool valid_dns_host(const char *host, const char *path, const char **port) {
    const char *end = host;
    while (end < path && *end != ':') end++;
    if (end == host || *host == '.' || *host == '-' || end[-1] == '.' || end[-1] == '-') return false;
    for (const char *p = host; p < end; p++) if (!valid_dns_character(p, host)) return false;
    if (end < path) *port = end + 1;
    return true;
}

static bool valid_port(const char *port, const char *path) {
    if (port) {
        unsigned value = 0;
        if (port == path) return false;
        for (const char *p = port; p < path; p++) {
            if (*p < '0' || *p > '9') return false;
            value = value * 10 + (unsigned)(*p - '0');
            if (value > 65535) return false;
        }
        if (!value) return false;
    }
    return true;
}

static bool valid_authority(const char *host, const char *path) {
    const char *port = NULL;
    bool host_valid = *host == '[' ? valid_ipv6_host(host, path, &port) : valid_dns_host(host, path, &port);
    return host_valid && valid_port(port, path);
}

static bool valid_server(const char *url) {
    if (!valid_url_text(url)) return false;
    if (!url[0]) return true;  // LAN mode: discovery finds the plugin
    const char *host;
    if (!strncmp(url, "kubik://", 8)) return url[8] && valid_authority(url + 8, url + strlen(url));
    if (!strncmp(url, "wss://", 6)) host = url + 6;
    else return false;
    const char *path = strchr(host, '/');
    return path && path != host && valid_authority(host, path);
}
bool settings_server_valid(const char *url) { return valid_server(url); }

static void load_server(nvs_handle_t h, settings_t *s) {
    server_settings_t pair = {0};
    size_t size = sizeof(pair);
    esp_err_t err = nvs_get_blob(h, "server", &pair, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        get_str(h, "url", pair.url, sizeof(pair.url), "");
        get_str(h, "token", pair.token, sizeof(pair.token), "");
    } else if (err != ESP_OK || size != sizeof(pair) || !memchr(pair.url, 0, sizeof(pair.url))) {
        return;
    }
    memset(pair.token, 0, sizeof pair.token);
    if (!valid_server(pair.url)) return;
    memcpy(s->server_url, pair.url, sizeof(pair.url));
}

typedef struct { char ssid[33], password[65], url[128]; } connection_settings_t;
typedef struct {
    uint32_t magic;
    uint8_t version, count, active, reserved;
    wifi_profile_t profiles[SETTINGS_WIFI_MAX];
    char url[128];
} saved_networks_t;

#define NETWORKS_MAGIC 0x4b574946u
#define NETWORKS_VERSION 1

static bool valid_profile(const wifi_profile_t *profile) {
    return memchr(profile->ssid, 0, sizeof profile->ssid) && profile->ssid[0] &&
           memchr(profile->password, 0, sizeof profile->password) &&
           (!profile->password[0] || strlen(profile->password) >= 8);
}

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

static bool valid_networks_header(const saved_networks_t *saved) {
    if (saved->magic != NETWORKS_MAGIC || saved->version != NETWORKS_VERSION ||
        saved->count > SETTINGS_WIFI_MAX || (saved->count && saved->active >= saved->count) ||
        (!saved->count && saved->active) || !memchr(saved->url, 0, sizeof saved->url) ||
        !valid_server(saved->url)) return false;
    return true;
}

static bool profile_duplicate(const saved_networks_t *saved, int index) {
    for (int i = 0; i < index; i++)
        if (!strcmp(saved->profiles[index].ssid, saved->profiles[i].ssid)) return true;
    return false;
}

static bool valid_networks(const saved_networks_t *saved) {
    if (!valid_networks_header(saved)) return false;
    for (int i = 0; i < saved->count; i++) {
        if (!valid_profile(&saved->profiles[i]) || profile_duplicate(saved, i)) return false;
    }
    return true;
}

static bool load_networks(nvs_handle_t h, settings_t *s, bool *present) {
    saved_networks_t saved = {0};
    size_t size = sizeof saved;
    esp_err_t err = nvs_get_blob(h, "networks", &saved, &size);
    *present = err != ESP_ERR_NVS_NOT_FOUND;
    if (err != ESP_OK || size != sizeof saved || !valid_networks(&saved)) return false;
    s->wifi_profile_count = saved.count;
    memcpy(s->wifi_profiles, saved.profiles, sizeof s->wifi_profiles);
    memcpy(s->server_url, saved.url, sizeof s->server_url);
    set_active_profile(s, saved.active);
    return true;
}

static bool load_connection(nvs_handle_t h, settings_t *s, bool *present) {
    connection_settings_t saved = {0};
    size_t size = sizeof saved;
    esp_err_t err = nvs_get_blob(h, "connection", &saved, &size);
    *present = err != ESP_ERR_NVS_NOT_FOUND;
    // A damaged current record must never revive stale legacy credentials.
    if (err != ESP_OK || size != sizeof saved ||
        !memchr(saved.ssid, 0, sizeof saved.ssid) || !memchr(saved.password, 0, sizeof saved.password) ||
        !memchr(saved.url, 0, sizeof saved.url) || !valid_server(saved.url) ||
        (saved.ssid[0] && saved.password[0] && strlen(saved.password) < 8)) return false;
    if (saved.ssid[0]) {
        s->wifi_profile_count = 1;
        memcpy(s->wifi_profiles[0].ssid, saved.ssid, sizeof saved.ssid);
        memcpy(s->wifi_profiles[0].password, saved.password, sizeof saved.password);
        set_active_profile(s, 0);
    }
    if (valid_server(saved.url)) memcpy(s->server_url, saved.url, sizeof saved.url);
    return true;
}

static void load_pin(nvs_handle_t h, settings_t *s) {
    size_t size = sizeof s->server_pin;
    s->server_pinned = nvs_get_blob(h, "pin", s->server_pin, &size) == ESP_OK && size == sizeof s->server_pin;
}

static void erase_old_connection(nvs_handle_t h) {
    static const char *const keys[] = {"connection", "wifi", "server", "ssid", "pass", "url", "token"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) nvs_erase_key(h, keys[i]);
    nvs_commit(h);
}

static void current_networks(const settings_t *s, saved_networks_t *saved) {
    memset(saved, 0, sizeof *saved);
    saved->magic = NETWORKS_MAGIC; saved->version = NETWORKS_VERSION; saved->count = s->wifi_profile_count;
    memcpy(saved->profiles, s->wifi_profiles, sizeof saved->profiles);
    for (int i = 0; i < s->wifi_profile_count; i++)
        if (!strcmp(s->wifi_profiles[i].ssid, s->wifi_ssid)) saved->active = i;
    snprintf(saved->url, sizeof saved->url, "%s", s->server_url);
}

static esp_err_t write_networks(nvs_handle_t h, const saved_networks_t *saved, bool forget_pin) {
    esp_err_t err = ESP_OK;
    if (forget_pin) {
        err = nvs_erase_key(h, "pin");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_set_blob(h, "networks", saved, sizeof *saved);
    if (err == ESP_OK) err = nvs_commit(h);
    return err;
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
    nvs_handle_t h;
    if (nvs_open("kubik", NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_str(h, "name", s->name);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "could not save English default name");
    nvs_close(h);
}

void settings_load(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_device_id, sizeof(g_device_id), "kubik-%02x%02x%02x", mac[3], mac[4], mac[5]);

    nvs_handle_t h;
    bool ok = nvs_open("kubik", NVS_READONLY, &h) == ESP_OK;
    settings_t *s = &g_settings;
    set_defaults(s);
    if (ok) {
        bool current_present = false, current_valid = load_networks(h, s, &current_present);
        bool migrate = false;
        if (!current_present) {
            bool connection_present = false;
            bool connection_valid = load_connection(h, s, &connection_present);
            if (!connection_present) { load_wifi(h, s); load_server(h, s); }
            load_pin(h, s);
            migrate = true;
            (void)connection_valid;  // a damaged current record blocks stale legacy credentials
        } else if (current_valid) {
            load_pin(h, s);
        } else {
            ESP_LOGW(TAG, "saved Wi-Fi list is damaged; legacy credentials were not restored");
        }
        get_str(h, "name", s->name, sizeof(s->name), "Kubik");
        s->volume = load_volume(h, "volume", 70);
        s->ui_volume = load_volume(h, "ui_volume", s->volume);
        s->brightness = get_int(h, "bright", 200);
        s->greeted = get_int(h, "greeted", 0);
        s->event_overlay = get_int(h, "event_overlay", 0) == 1;
        s->guide_done = get_int(h, "guide_done", 0) == 1;
        nvs_close(h);
        if (migrate && nvs_open("kubik", NVS_READWRITE, &h) == ESP_OK) {
            saved_networks_t saved;
            current_networks(s, &saved);
            if (write_networks(h, &saved, false) == ESP_OK) erase_old_connection(h);
            else ESP_LOGW(TAG, "could not commit migrated Wi-Fi list; legacy data was kept");
            nvs_close(h);
        }
    }
    migrate_default_name(s);
    ESP_LOGI(TAG, "device %s, Wi-Fi profiles %u, server %s", g_device_id, s->wifi_profile_count,
             s->server_url[0] ? "set" : "unset");
}

void settings_save(void) {
    nvs_handle_t h;
    if (nvs_open("kubik", NVS_READWRITE, &h) != ESP_OK) return;
    settings_t *s = &g_settings;
    // General preferences never rewrite connection credentials.
    nvs_set_str(h, "name", s->name);
    nvs_set_i32(h, "volume", s->volume);
    nvs_set_i32(h, "ui_volume", s->ui_volume);
    nvs_set_i32(h, "bright", s->brightness);
    nvs_set_i32(h, "greeted", s->greeted);
    nvs_set_i32(h, "event_overlay", s->event_overlay);
    nvs_erase_key(h, "character");  // now a firmware build choice
    nvs_erase_key(h, "tiltgain");  // the tilt tuning is gone (a fixed 0.4)
    nvs_erase_key(h, "tiltinv");
    nvs_erase_key(h, "vengine");  // the voice choice is gone (one fixed STT, agent, TTS pipeline): frees old keys
    nvs_erase_key(h, "vname");
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t settings_complete_guide(void) {
    if (g_settings.guide_done) return ESP_OK;
    nvs_handle_t h;
    esp_err_t err = nvs_open("kubik", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(h, "guide_done", 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) g_settings.guide_done = true;
    return err;
}

bool settings_take_provisioning(void) {
    nvs_handle_t h;
    if (nvs_open("kubik", NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t requested = 0;
    nvs_get_u8(h, "setup", &requested);
    esp_err_t err = ESP_OK;
    if (requested) {
        err = nvs_erase_key(h, "setup");
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);
    // Never enter setup if the one-shot flag could not be consumed.
    return requested && err == ESP_OK;
}

static esp_err_t upsert_network(const settings_t *current, saved_networks_t *saved,
                                const char *ssid, const char *password, int *active_index);

static bool valid_connection(const char *ssid, const char *password, const char *url) {
    return ssid && password && url && ssid[0] && strlen(ssid) <= 32 && strlen(password) <= 64 &&
        valid_server(url) && g_settings.wifi_profile_count <= SETTINGS_WIFI_MAX;
}

esp_err_t settings_save_connection(const char *ssid, const char *password, const char *url) {
    if (!valid_connection(ssid, password, url)) return ESP_ERR_INVALID_ARG;
    // NVS/formatting already use a deep stack. USB config must not add the eight-profile snapshot to it.
    saved_networks_t *saved = malloc(sizeof *saved);
    if (!saved) return ESP_ERR_NO_MEM;
    current_networks(&g_settings, saved);
    int active_index = -1;
    esp_err_t err = upsert_network(&g_settings, saved, ssid, password, &active_index);
    if (err == ESP_OK) {
        snprintf(saved->url, sizeof saved->url, "%s", url);
        nvs_handle_t h;
        err = nvs_open("kubik", NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = write_networks(h, saved, true);
            if (err == ESP_OK) erase_old_connection(h);
            nvs_close(h);
        }
    }
    if (err == ESP_OK) {
        g_settings.wifi_profile_count = saved->count;
        memcpy(g_settings.wifi_profiles, saved->profiles, sizeof g_settings.wifi_profiles);
        set_active_profile(&g_settings, active_index);
        memcpy(g_settings.server_url, saved->url, sizeof g_settings.server_url);
        memset(g_settings.server_pin, 0, sizeof g_settings.server_pin);
        g_settings.server_pinned = false;
    }
    volatile unsigned char *secret = (volatile unsigned char *)saved;
    for (size_t i = 0; i < sizeof *saved; i++) secret[i] = 0;
    free(saved);
    return err;
}

esp_err_t settings_save_pin(const uint8_t pin[SETTINGS_PIN_BYTES]) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("kubik", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "pin", pin, SETTINGS_PIN_BYTES);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        memcpy(g_settings.server_pin, pin, SETTINGS_PIN_BYTES);
        g_settings.server_pinned = true;
    }
    return err;
}

esp_err_t settings_factory_reset(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("kubik", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) set_defaults(&g_settings);
    return err;
}

static esp_err_t upsert_network(const settings_t *current, saved_networks_t *saved,
                                const char *ssid, const char *password, int *active_index) {
    int index = settings_wifi_find(ssid);
    char chosen_password[65] = {0};
    if (password[0]) snprintf(chosen_password, sizeof chosen_password, "%s", password);
    else if (index >= 0) snprintf(chosen_password, sizeof chosen_password, "%s", current->wifi_profiles[index].password);
    if (chosen_password[0] && strlen(chosen_password) < 8) return ESP_ERR_INVALID_ARG;
    if (index < 0) {
        if (current->wifi_profile_count == SETTINGS_WIFI_MAX) return ESP_ERR_NO_MEM;
        index = saved->count++;
    }
    snprintf(saved->profiles[index].ssid, sizeof saved->profiles[index].ssid, "%s", ssid);
    snprintf(saved->profiles[index].password, sizeof saved->profiles[index].password, "%s", chosen_password);
    saved->active = index;
    *active_index = index;
    return ESP_OK;
}
