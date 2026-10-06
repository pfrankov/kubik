#include "muse_pair.h"
#include "muse_ble.h"
#include "muse_store.h"
#include "muse_json.h"
#include "esp_timer.h"
#include "link_pairing.h"
#include "settings.h"
#include "version.h"
#include "wifi.h"
#include "app_internal.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "cJSON.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static muse_credentials_t *credentials;
static char node[32], device[48], mac_text[18], name[32];
static SemaphoreHandle_t send_lock;
static bool active;

bool muse_pair_active(void) { return active; }

static void card(const char *text) {
    face_lock(); face_card(&g_face, text); face_unlock();
    disp_wake();
}

static bool send_status(const char *status, uint32_t generation) {
    xSemaphoreTake(send_lock, portMAX_DELAY);
    uint32_t record = 0;
    char *json = link_pairing_encrypt_status(status, generation, &record);
    bool sent = json && muse_ble_send(json, record);
    if (json) { muse_store_wipe(json, strlen(json)); free(json); }
    xSemaphoreGive(send_lock); return sent;
}

static void send_object(cJSON *object, bool encrypted, uint32_t generation) {
    char *plain = cJSON_PrintUnformatted(object);
    if (!plain) return;
    xSemaphoreTake(send_lock, portMAX_DELAY);
    uint32_t record = 0;
    char *json = encrypted ? link_pairing_encrypt_json(plain, generation, &record) : NULL;
    if (!encrypted || json) muse_ble_send(encrypted ? json : plain, record);
    if (json) { muse_store_wipe(json, strlen(json)); free(json); }
    muse_store_wipe(plain, strlen(plain)); free(plain);
    xSemaphoreGive(send_lock);
}

static void device_info(void) {
    cJSON *object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "type", "device_info");
    cJSON_AddStringToObject(object, "node_id", node);
    cJSON_AddStringToObject(object, "version", KUBIK_FW_VERSION);
    link_pairing_add_device_info(object);
    send_object(object, false, 0); cJSON_Delete(object);
}

static void scan(uint32_t generation) {
    wifi_net_t networks[12];
    int count = wifi_scan(networks, 12);
    cJSON *object = cJSON_CreateObject(), *array = cJSON_AddArrayToObject(object, "networks");
    cJSON_AddStringToObject(object, "type", "wifi_scan_result");
    for (int i = 0; i < count; i++) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", networks[i].rssi);
        cJSON_AddBoolToObject(network, "secure", networks[i].secure);
        cJSON_AddItemToArray(array, network);
    }
    send_object(object, true, generation); cJSON_Delete(object);
}

static const char *field(cJSON *object, const char *key, size_t cap, bool empty) {
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
    return value && strlen(value) < cap && (empty || value[0]) ? value : NULL;
}
static const char *provision_ssid, *provision_password;
static bool commit_credentials(void) {
    int saved = settings_wifi_find(provision_ssid);
    bool same = saved >= 0 && !strcmp(g_settings.wifi_profiles[saved].password, provision_password);
    esp_err_t err = same ? ESP_OK : settings_save_connection(provision_ssid, provision_password, g_settings.server_url);
    return err == ESP_OK && muse_store_save(credentials) == ESP_OK;
}
static bool endpoint_supported(cJSON *object, const char *key, const char *expected) {
    cJSON *entry = cJSON_GetObjectItemCaseSensitive(object, key);
    const char *value = cJSON_GetStringValue(entry);
    return !entry || (value && (!value[0] || !strcmp(value, expected)));
}
static bool join_wifi(const char *ssid, const char *password, uint32_t generation) {
    if (!send_status("wifi_connecting", generation)) return false;
    wifi_try(ssid, password);
    int64_t deadline = esp_timer_get_time() + 25000000;
    while (!wifi_sta_connected() && wifi_attempts() < 3 && esp_timer_get_time() < deadline) {
        if (!link_pairing_provisioning_session_valid(generation)) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return wifi_sta_connected() && link_pairing_provisioning_session_valid(generation) &&
        send_status("wifi_connected", generation);
}
static void provision(cJSON *object) {
    const char *ssid = field(object, "ssid", 33, false);
    const char *password = field(object, "password", 65, true);
    const char *access = field(object, "access_token", MUSE_TOKEN_CAP, false);
    const char *refresh = field(object, "refresh_token", MUSE_TOKEN_CAP, false);
    const char *type = field(object, "token_type", 16, false);
    bool endpoints = endpoint_supported(object, "api_url_v2", "https://api.muse.ai") &&
        endpoint_supported(object, "noise_host", "hatch.metaaivm.com");
    if (!ssid || !password || !muse_account_token_valid(access) || !muse_account_token_valid(refresh) ||
        !type || strcmp(type, "device") || !endpoints) {
        send_status("error_invalid_command", link_pairing_session_generation()); return;
    }
    uint32_t generation = link_pairing_mark_provisioning_active();
    if (!generation) return;
    if (!join_wifi(ssid, password, generation)) {
        send_status("wifi_failed", generation); wifi_try(NULL, NULL); return;
    }
    snprintf(credentials->access_token, sizeof credentials->access_token, "%s", access);
    snprintf(credentials->refresh_token, sizeof credentials->refresh_token, "%s", refresh);
    credentials->state = MUSE_PAIRED; provision_ssid = ssid; provision_password = password;
    bool committed = link_pairing_commit_provisioning(generation, commit_credentials);
    provision_ssid = provision_password = NULL;
    if (!committed) {
        credentials->state = MUSE_PAIRING;
        muse_store_wipe(credentials->access_token, sizeof credentials->access_token);
        muse_store_wipe(credentials->refresh_token, sizeof credentials->refresh_token);
        send_status("auth_failed", generation); wifi_try(NULL, NULL); return;
    }
    send_status("auth_ok", generation);
    card("Muse account paired. Connecting...");
    vTaskDelay(pdMS_TO_TICKS(2000)); esp_restart();
}

static void dispatch_command(const char *command, bool encrypted, uint32_t epoch);
static void encrypted_command(cJSON *object, uint32_t epoch) {
    char *plain = NULL;
    if (link_pairing_decrypt_command(object, &plain) || !plain) { muse_ble_disconnect(); return; }
    if (muse_ble_current(epoch)) dispatch_command(plain, true, epoch);
    muse_store_wipe(plain, strlen(plain)); free(plain);
}

static void finish_handshake(cJSON *object) {
    if (!object->child || object->child->next) { muse_ble_disconnect(); return; }
    uint32_t generation = link_pairing_handle_client_finished();
    if (!generation) { muse_ble_disconnect(); return; }
    if (!send_status("confirm_required", generation)) { muse_ble_disconnect(); return; }
    if (link_pairing_arm_confirmation(generation)) card("Confirm Muse pairing\nPress KEY on Kubik.");
}

static void dispatch_plain(cJSON *object, const char *action, uint32_t epoch) {
    if (!strcmp(action, "pairing_encrypted")) { encrypted_command(object, epoch); return; }
    if (!strcmp(action, "pairing_client_hello")) {
        char *reply = NULL;
        if (!link_pairing_handle_client_hello(object, &reply) && reply) {
            muse_ble_send(reply, 0); free(reply);
        } else muse_ble_disconnect();
    } else if (!strcmp(action, "get_device_info") && !link_pairing_session_generation()) device_info();
    // Plaintext provisioning, scanning and control commands are never accepted.
}

static void dispatch_command(const char *command, bool encrypted, uint32_t epoch) {
    if (!muse_ble_current(epoch)) return;
    cJSON *object = muse_json_parse(command, strlen(command));
    const char *action = field(object, "action", 64, false);
    if (!action) { muse_json_clear(object); return; }
    if (!encrypted) dispatch_plain(object, action, epoch);
    else if (!strcmp(action, "pairing_client_finished")) finish_handshake(object);
    else if (link_pairing_session_confirmed()) {
        uint32_t generation = link_pairing_session_generation();
        if (!strcmp(action, "wifi_scan")) scan(generation);
        else if (!strcmp(action, "provision_v2")) provision(object);
        else send_status("error_invalid_command", generation);
    }
    muse_json_clear(object);
}

bool muse_pair_key(void) {
    if (!active) return false;
    uint32_t generation = link_pairing_confirm_active_session();
    if (generation && send_status("pairing_confirmed", generation))
        card("Continue in the Muse app\nChoose Wi-Fi to finish pairing.");
    return true;
}

esp_err_t muse_pair_start(void) {
    credentials = calloc(1, sizeof *credentials); send_lock = xSemaphoreCreateMutex();
    if (!credentials || !send_lock) return ESP_ERR_NO_MEM;
    esp_err_t err = muse_store_load(credentials);
    if (err != ESP_OK || credentials->state != MUSE_PAIRING) return ESP_ERR_INVALID_STATE;
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_text, sizeof mac_text, "%02x:%02x:%02x:%02x:%02x:%02x", MAC2STR(mac));
    snprintf(node, sizeof node, "homelink-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(device, sizeof device, "hatch-link:%s", mac_text);
    snprintf(name, sizeof name, "MuseGadget-%02X%02X%02X", mac[3], mac[4], mac[5]);
    link_pairing_init(node, device, mac_text, KUBIK_FW_VERSION, credentials->sdk_token);
    active = true;
    char prompt[180];
    snprintf(prompt, sizeof prompt, "Pair Muse on your phone\n%s\nMuse: Settings > Devices > Developer mode > Add device.", name);
    card(prompt);
    err = muse_ble_start(name, dispatch_command);
    if (err != ESP_OK) { active = false; card("Muse pairing unavailable. Open Settings to try again."); }
    return err;
}
