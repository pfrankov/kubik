#include "muse_control.h"
#include "muse_vm.h"
#include "muse_json.h"
#include "muse_link.h"
#include "settings.h"
#include "version.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// /link-control carries little-endian length-prefixed JSON, not chat NDJSON.
#define CONTROL_MAX 4096
static char registration[40];
static uint8_t *incoming;
static size_t received;
static int64_t stream, started;
static bool ready, failed;
static void (*notify)(const char *);

static const char *string(const cJSON *object, const char *key) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}
static bool equals(const char *value, const char *expected) { return value && !strcmp(value, expected); }
static uint32_t length(const uint8_t *bytes) {
    return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}
static bool send(cJSON *object) {
    char *json = cJSON_PrintUnformatted(object); cJSON_Delete(object);
    if (!json) return false;
    size_t size = strlen(json); uint8_t *framed = malloc(size + 4);
    bool ok = framed && size <= 2400;
    if (ok) {
        for (unsigned i = 0; i < 4; i++) framed[i] = (uint8_t)(size >> (i * 8));
        memcpy(framed + 4, json, size); ok = muse_link_req_send(stream, framed, size + 4, false, 200);
    }
    free(framed); free(json); return ok;
}
static void invoke(const cJSON *request) {
    const char *id = string(request, "id"), *command = string(request, "command");
    if (!id || strlen(id) > 80 || !command) return;
    const char *text = string(cJSON_GetObjectItemCaseSensitive(request, "params"), "text");
    bool accepted = equals(command, "device.notify") && text && text[0] && strlen(text) < 1024;
    if (accepted && notify) notify(text);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "method", "link.result"); cJSON_AddStringToObject(response, "id", id);
    cJSON_AddBoolToObject(response, "ok", accepted);
    if (accepted) cJSON_AddObjectToObject(response, "payload");
    else cJSON_AddStringToObject(response, "error", "Only device.notify with short text is supported");
    if (!send(response)) failed = true;
}
static void message(const char *bytes, size_t size) {
    cJSON *object = muse_json_parse(bytes, size);
    if (!object) { failed = true; return; }
    if (equals(string(object, "id"), registration) && equals(string(object, "type"), "res")) {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(object, "error");
        cJSON *result = cJSON_GetObjectItemCaseSensitive(object, "result");
        bool rejected = error && !cJSON_IsNull(error) && !cJSON_IsFalse(error);
        ready = !rejected && equals(string(result, "status"), "registered");
        failed = !ready;
    } else if (ready && equals(string(object, "method"), "link.invoke")) invoke(object);
    else if (equals(string(object, "type"), "event")) {
        const char *event = string(object, "event");
        if (equals(event, "link.unpaired") || equals(event, "node.unpaired")) failed = true;
    }
    cJSON_Delete(object);
}
static size_t expected_bytes(void) {
    if (received < 4) return 4;
    uint32_t size = length(incoming);
    if (size > CONTROL_MAX - 4) { failed = true; return 0; }
    return size + 4;
}
static void on_frame(void *context, int status, const uint8_t *data, size_t size, bool end) {
    (void)context;
    if (status < 0 || status >= 400 || end || !incoming) { failed = true; return; }
    // A service chunk can contain several messages; cap each framed message,
    // rather than rejecting an otherwise valid coalesced WebSocket burst.
    while (size && !failed) {
        size_t expected = expected_bytes();
        if (failed) return;
        size_t take = expected - received;
        if (take > size) take = size;
        memcpy(incoming + received, data, take); received += take; data += take; size -= take;
        if (received == 4) expected = expected_bytes();
        if (!failed && received == expected) {
            if (expected > 4) message((const char *)incoming + 4, expected - 4);
            received = 0;
        }
    }
}
static cJSON *register_request(void) {
    cJSON *object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "type", "req"); cJSON_AddStringToObject(object, "id", registration);
    cJSON_AddStringToObject(object, "method", "link.register");
    cJSON *params = cJSON_AddObjectToObject(object, "params"); char node[32], device[48];
    muse_device_identity(node, sizeof node, device, sizeof device);
    cJSON_AddStringToObject(params, "node_id", node);
    cJSON_AddStringToObject(params, "display_name", g_settings.name);
    cJSON_AddStringToObject(params, "platform", "esp32"); cJSON_AddStringToObject(params, "version", KUBIK_FW_VERSION);
    cJSON_AddStringToObject(params, "device_family", "link"); cJSON_AddStringToObject(params, "model_id", "kubik-c6");
    cJSON_AddBoolToObject(params, "is_wakeup_supported", false);
    cJSON *commands = cJSON_AddObjectToObject(params, "commands");
    cJSON *command = cJSON_AddObjectToObject(commands, "device.notify");
    cJSON_AddStringToObject(command, "description", "Wake Kubik and display a text notification, up to 1023 UTF-8 bytes");
    cJSON *required = cJSON_AddObjectToObject(command, "required"), *text = cJSON_AddObjectToObject(required, "text");
    cJSON_AddStringToObject(text, "type", "string"); cJSON_AddStringToObject(text, "description", "Notification text");
    cJSON_AddObjectToObject(command, "optional");
    return object;
}
bool muse_control_start(void (*notification)(const char *)) {
    if (stream) return true;
    notify = notification; incoming = malloc(CONTROL_MAX);
    if (!incoming) { failed = true; return false; }
    snprintf(registration, sizeof registration, "kubik-%08lx-%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    stream = muse_link_req_open("POST", "/link-control", NULL, false, on_frame, NULL);
    if (!stream) { failed = true; return false; }
    started = esp_timer_get_time();
    if (!send(register_request())) { failed = true; return false; }
    return true;
}
bool muse_control_ready(void) { return ready && !failed; }
bool muse_control_failed(void) {
    return failed || (stream && !ready && esp_timer_get_time() - started > 20000000);
}
void muse_control_clear(void) {
    if (stream) muse_link_req_cancel(stream);
    stream = 0; ready = failed = false; received = 0; registration[0] = 0;
    free(incoming); incoming = NULL;
}
