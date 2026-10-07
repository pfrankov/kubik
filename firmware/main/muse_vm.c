#include "muse_vm.h"
#include "muse_store.h"
#include "muse_json.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "cJSON.h"

#define RESPONSE_MAX 16384
typedef struct { char *bytes; size_t size; bool overflow; } response_t;

void muse_device_identity(char *node, unsigned node_cap, char *device, unsigned device_cap) {
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(node, node_cap, "homelink-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(device, device_cap, "hatch-link:%02x:%02x:%02x:%02x:%02x:%02x", MAC2STR(mac));
}
static esp_err_t collect(esp_http_client_event_t *event) {
    response_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (event->data_len < 0 || response->size + event->data_len >= RESPONSE_MAX) {
        response->overflow = true; return ESP_FAIL;
    }
    memcpy(response->bytes + response->size, event->data, event->data_len);
    response->size += event->data_len; response->bytes[response->size] = 0;
    return ESP_OK;
}

static cJSON *request(const char *path, const char *bearer, const char *body, int *status) {
    response_t response = {.bytes=calloc(1, RESPONSE_MAX)};
    if (!response.bytes) return NULL;
    char url[128]; snprintf(url, sizeof url, "https://api.muse.ai%s", path);
    esp_http_client_config_t config = {.url=url,.crt_bundle_attach=esp_crt_bundle_attach,
        .timeout_ms=12000,.disable_auto_redirect=true,.event_handler=collect,.user_data=&response,
        .buffer_size=1024,.buffer_size_tx=2048};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (client) {
        esp_http_client_set_header(client, "Authorization", bearer);
        esp_http_client_set_header(client, "X-API-Version", "1.0.0");
        if (body) {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, body, strlen(body));
        }
        err = esp_http_client_perform(client);
        *status = esp_http_client_get_status_code(client); esp_http_client_cleanup(client);
    }
    cJSON *json = err == ESP_OK && !response.overflow ? muse_json_parse(response.bytes, response.size) : NULL;
    muse_store_wipe(response.bytes, RESPONSE_MAX); free(response.bytes);
    return json;
}
static bool copy_field(cJSON *object, const char *key, char *out, size_t cap) {
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
    if (!value || !value[0] || strlen(value) >= cap) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) if (*p < 32 || *p >= 127) return false;
    memcpy(out, value, strlen(value) + 1); return true;
}
static bool refresh(muse_credentials_t *credentials, uint32_t generation, char *auth, size_t cap) {
    const char *raw = strrchr(credentials->refresh_token, ':');
    snprintf(auth, cap, "Bearer hatch_refresh:%s", raw ? raw + 1 : credentials->refresh_token);
    cJSON *body = cJSON_CreateObject(); char node[32], device[48];
    muse_device_identity(node, sizeof node, device, sizeof device);
    // Upstream token mint/refresh binds device_id to the node identity.
    cJSON_AddStringToObject(body, "device_id", node);
    cJSON_AddStringToObject(body, "sdk_token", credentials->sdk_token);
    char *serialized = cJSON_PrintUnformatted(body); muse_json_clear(body);
    if (!serialized) return false;
    int status = 0; cJSON *response = request("/device_token/refresh", auth, serialized, &status);
    muse_store_wipe(serialized, strlen(serialized)); free(serialized);
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(response, "payload");
    cJSON *result = cJSON_IsObject(payload) ? payload : response;
    bool ok = status == 200 && copy_field(result, "access_token", credentials->access_token, sizeof credentials->access_token) &&
        copy_field(result, "refresh_token", credentials->refresh_token, sizeof credentials->refresh_token);
    if (ok) ok = muse_store_refresh_save(credentials, generation) == ESP_OK;
    muse_json_clear(response); return ok;
}

static bool select_vm(cJSON *response, muse_vm_t *out) {
    cJSON *vms = cJSON_GetObjectItemCaseSensitive(response, "vm_list");
    if (!cJSON_IsArray(vms) || cJSON_GetArraySize(vms) > 32) return false;
    cJSON *chosen = cJSON_GetArrayItem(vms, 0), *item;
    cJSON_ArrayForEach(item, vms) if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "default"))) { chosen = item; break; }
    if (!copy_field(chosen, "vm_id", out->id, sizeof out->id) ||
        !copy_field(chosen, "vm_auth_token", out->token, sizeof out->token)) return false;
    // The VM id is a query value. Never allow header/path injection.
    for (const unsigned char *p = (const unsigned char *)out->id; *p; p++)
        if (!isalnum(*p) && *p != '-' && *p != '_') return false;
    return true;
}

esp_err_t muse_vm_lookup(muse_vm_t *out) {
    muse_credentials_t *credentials = calloc(1, sizeof *credentials);
    char *auth = calloc(1, MUSE_TOKEN_CAP + 32);
    if (!credentials || !auth) { free(credentials); free(auth); return ESP_ERR_NO_MEM; }
    int status = 0; cJSON *response = NULL;
    uint32_t generation = 0;
    esp_err_t err = muse_store_load_generation(credentials, &generation, NULL);
    if (err == ESP_OK && credentials->state != MUSE_PAIRED) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        snprintf(auth, MUSE_TOKEN_CAP + 32, "Bearer %s", credentials->access_token);
        response = request("/fetch_vms", auth, NULL, &status);
        if (status == 401 && refresh(credentials, generation, auth, MUSE_TOKEN_CAP + 32)) {
            muse_json_clear(response);
            snprintf(auth, MUSE_TOKEN_CAP + 32, "Bearer %s", credentials->access_token);
            response = request("/fetch_vms", auth, NULL, &status);
        }
        err = status == 200 && select_vm(response, out) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    muse_json_clear(response);
    muse_store_wipe(credentials, sizeof *credentials); free(credentials);
    muse_store_wipe(auth, MUSE_TOKEN_CAP + 32); free(auth);
    if (err != ESP_OK) muse_store_wipe(out, sizeof *out);
    return err;
}
