#include "connection_store_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "lwip/inet.h"
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#endif
#include "nvs.h"
#include "mbedtls/sha256.h"

#define LEGACY_NETWORKS_MAGIC 0x4b574946u
#define LEGACY_NETWORKS_VERSION 1u

typedef struct { char ssid[33]; char password[65]; } legacy_wifi_t;
typedef struct { char url[128]; char token[80]; } legacy_server_t;
typedef struct { char ssid[33], password[65], url[128]; } legacy_connection_t;
typedef struct {
    uint32_t magic;
    uint8_t version, count, active, reserved;
    wifi_profile_t profiles[SETTINGS_WIFI_MAX];
    char url[128];
} legacy_networks_t;

static bool zero_bytes(const void *data, size_t size) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; i++) if (bytes[i]) return false;
    return true;
}

static bool valid_url_text(const char *url) {
    if (!url || strnlen(url, 128) >= 128) return false;
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

bool connection_store_server_valid(const char *url) {
    if (!valid_url_text(url)) return false;
    if (!url[0]) return true;
    const char *host;
    if (!strncmp(url, "kubik://", 8)) return url[8] && valid_authority(url + 8, url + strlen(url));
    if (!strncmp(url, "wss://", 6)) host = url + 6;
    else return false;
    const char *path = strchr(host, '/');
    return path && path != host && valid_authority(host, path);
}

static bool valid_profile(const wifi_profile_t *profile) {
    return memchr(profile->ssid, 0, sizeof profile->ssid) && profile->ssid[0] &&
           memchr(profile->password, 0, sizeof profile->password) &&
           (!profile->password[0] || strlen(profile->password) >= 8);
}

static bool profile_duplicate(const connection_networks_t *networks, int index) {
    for (int i = 0; i < index; i++)
        if (!strcmp(networks->profiles[index].ssid, networks->profiles[i].ssid)) return true;
    return false;
}

static bool valid_network_shape(const connection_networks_t *networks) {
    return networks->magic == LEGACY_NETWORKS_MAGIC && networks->version == LEGACY_NETWORKS_VERSION &&
        networks->count <= SETTINGS_WIFI_MAX &&
        (networks->count ? networks->active < networks->count : !networks->active);
}

static bool valid_network_profiles(const connection_networks_t *networks) {
    for (int i = 0; i < networks->count; i++)
        if (!valid_profile(&networks->profiles[i]) || profile_duplicate(networks, i)) return false;
    return true;
}

bool connection_store_networks_valid(const connection_networks_t *networks) {
    return networks && valid_network_shape(networks) &&
        memchr(networks->url, 0, sizeof networks->url) && connection_store_server_valid(networks->url) &&
        valid_network_profiles(networks);
}

bool connection_store_record_contents_valid(const connection_record_t *record) {
    if (record->magic != CONNECTION_RECORD_MAGIC || record->version != CONNECTION_RECORD_VERSION ||
        (record->flags & ~CONNECTION_RECORD_PIN_SET) || !connection_store_networks_valid(&record->networks)) return false;
    if (!(record->flags & CONNECTION_RECORD_PIN_SET) && !zero_bytes(record->server_pin, sizeof record->server_pin))
        return false;
    if (record->muse.magic == 0) return zero_bytes(&record->muse, sizeof record->muse);
    return muse_store_credentials_valid(&record->muse);
}

bool connection_store_record_valid(const connection_record_t *record) {
    return record->revision != 0 && record->network_revision <= record->revision &&
        connection_store_record_contents_valid(record);
}

void connection_store_record_defaults(connection_record_t *record) {
    memset(record, 0, sizeof *record);
    record->magic = CONNECTION_RECORD_MAGIC;
    record->version = CONNECTION_RECORD_VERSION;
    record->networks.magic = LEGACY_NETWORKS_MAGIC;
    record->networks.version = LEGACY_NETWORKS_VERSION;
}

static esp_err_t get_legacy_string(nvs_handle_t handle, const char *key, char *out,
                                   size_t capacity, bool *present) {
    size_t length = capacity;
    esp_err_t err = nvs_get_str(handle, key, out, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(out, 0, capacity);
        *present = false;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    if (!memchr(out, 0, capacity)) return ESP_ERR_INVALID_STATE;
    *present = true;
    return ESP_OK;
}

static esp_err_t load_legacy_wifi_blob(nvs_handle_t handle, connection_networks_t *networks) {
    legacy_wifi_t pair = {0};
    size_t size = sizeof pair;
    esp_err_t err = nvs_get_blob(handle, "wifi", &pair, &size);
    if (err == ESP_OK) {
        bool valid = size == sizeof pair && memchr(pair.ssid, 0, sizeof pair.ssid) &&
            memchr(pair.password, 0, sizeof pair.password) &&
            !(pair.ssid[0] && pair.password[0] && strlen(pair.password) < 8);
        if (!valid) err = ESP_ERR_INVALID_STATE;
        else if (pair.ssid[0]) {
            networks->count = 1;
            memcpy(networks->profiles[0].ssid, pair.ssid, sizeof pair.ssid);
            memcpy(networks->profiles[0].password, pair.password, sizeof pair.password);
        }
    }
    muse_store_wipe(&pair, sizeof pair);
    return err;
}

static esp_err_t load_legacy_wifi_strings(nvs_handle_t handle, connection_networks_t *networks) {
    bool ssid_present = false, password_present = false;
    esp_err_t err = get_legacy_string(handle, "ssid", networks->profiles[0].ssid,
                                      sizeof networks->profiles[0].ssid, &ssid_present);
    if (err == ESP_OK)
        err = get_legacy_string(handle, "pass", networks->profiles[0].password,
                                sizeof networks->profiles[0].password, &password_present);
    if (err == ESP_OK && ssid_present && networks->profiles[0].ssid[0]) {
        if (networks->profiles[0].password[0] && strlen(networks->profiles[0].password) < 8)
            err = ESP_ERR_INVALID_STATE;
        else networks->count = 1;
    }
    if (err != ESP_OK || !ssid_present || !networks->profiles[0].ssid[0])
        memset(&networks->profiles[0], 0, sizeof networks->profiles[0]);
    return err;
}

static esp_err_t load_legacy_wifi(nvs_handle_t handle, connection_networks_t *networks) {
    esp_err_t err = load_legacy_wifi_blob(handle, networks);
    return err == ESP_ERR_NVS_NOT_FOUND ? load_legacy_wifi_strings(handle, networks) : err;
}

static esp_err_t load_legacy_server(nvs_handle_t handle, connection_networks_t *networks) {
    legacy_server_t pair = {0};
    size_t size = sizeof pair;
    esp_err_t err = nvs_get_blob(handle, "server", &pair, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        bool present = false;
        err = get_legacy_string(handle, "url", pair.url, sizeof pair.url, &present);
        if (err == ESP_OK && present && !connection_store_server_valid(pair.url)) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK && present) memcpy(networks->url, pair.url, sizeof pair.url);
        muse_store_wipe(&pair, sizeof pair);
        return err;
    }
    if (err != ESP_OK) {
        muse_store_wipe(&pair, sizeof pair);
        return err;
    }
    if (size != sizeof pair || !memchr(pair.url, 0, sizeof pair.url) ||
        !connection_store_server_valid(pair.url)) err = ESP_ERR_INVALID_STATE;
    else memcpy(networks->url, pair.url, sizeof pair.url);
    muse_store_wipe(&pair, sizeof pair);
    return err;
}

static esp_err_t load_legacy_muse(nvs_handle_t handle, connection_record_t *record) {
    size_t size = sizeof record->muse;
    esp_err_t err = nvs_get_blob(handle, "muse_v1", &record->muse, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (size == sizeof record->muse && muse_store_credentials_valid(&record->muse)) {
        record->muse_generation = 1;
        return ESP_OK;
    } else {
        memset(&record->muse, 0, sizeof record->muse);
        return ESP_ERR_INVALID_STATE;
    }
}

static esp_err_t load_legacy_pin(nvs_handle_t handle, connection_record_t *record) {
    size_t size = sizeof record->server_pin;
    esp_err_t err = nvs_get_blob(handle, "pin", record->server_pin, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (size == sizeof record->server_pin)
        record->flags |= CONNECTION_RECORD_PIN_SET;
    else return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static esp_err_t load_legacy_networks(nvs_handle_t handle, connection_record_t *record) {
    legacy_networks_t saved = {0};
    size_t size = sizeof saved;
    esp_err_t err = nvs_get_blob(handle, "networks", &saved, &size);
    if (err == ESP_OK) {
        connection_networks_t candidate;
        if (size == sizeof saved) {
            memcpy(&candidate, &saved, sizeof candidate);
            if (connection_store_networks_valid(&candidate))
                memcpy(&record->networks, &candidate, sizeof candidate);
            else err = ESP_ERR_INVALID_STATE;
        } else err = ESP_ERR_INVALID_STATE;
    }
    muse_store_wipe(&saved, sizeof saved);
    return err;
}

static esp_err_t load_legacy_connection(nvs_handle_t handle, connection_record_t *record) {
    legacy_connection_t connection = {0};
    size_t size = sizeof connection;
    esp_err_t err = nvs_get_blob(handle, "connection", &connection, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = load_legacy_wifi(handle, &record->networks);
        if (err == ESP_OK) err = load_legacy_server(handle, &record->networks);
    } else if (err == ESP_OK) {
        bool valid = size == sizeof connection && memchr(connection.ssid, 0, sizeof connection.ssid) &&
            memchr(connection.password, 0, sizeof connection.password) &&
            memchr(connection.url, 0, sizeof connection.url) &&
            connection_store_server_valid(connection.url) &&
            !(connection.ssid[0] && connection.password[0] && strlen(connection.password) < 8);
        if (!valid) err = ESP_ERR_INVALID_STATE;
        else {
            if (connection.ssid[0]) {
                record->networks.count = 1;
                memcpy(record->networks.profiles[0].ssid, connection.ssid, sizeof connection.ssid);
                memcpy(record->networks.profiles[0].password, connection.password, sizeof connection.password);
            }
            memcpy(record->networks.url, connection.url, sizeof connection.url);
        }
    }
    muse_store_wipe(&connection, sizeof connection);
    return err;
}

esp_err_t connection_store_load_legacy(nvs_handle_t handle, connection_record_t *record) {
    esp_err_t err = load_legacy_networks(handle, record);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = load_legacy_connection(handle, record);
    if (err == ESP_OK) err = load_legacy_pin(handle, record);
    if (err == ESP_OK) err = load_legacy_muse(handle, record);
    return err;
}

esp_err_t connection_store_digest_record(const connection_record_t *record, record_digest_t *digest) {
    return mbedtls_sha256((const unsigned char *)record, sizeof *record, digest->digest, 0) == 0
        ? ESP_OK : ESP_FAIL;
}

bool connection_store_digest_matches(const connection_record_t *record, const record_digest_t *digest) {
    uint8_t actual[32];
    if (mbedtls_sha256((const unsigned char *)record, sizeof *record, actual, 0) != 0) return false;
    return memcmp(actual, digest->digest, sizeof actual) == 0;
}
