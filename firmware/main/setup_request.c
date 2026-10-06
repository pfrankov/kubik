#include "setup_request.h"
#include "muse_json.h"
#include "muse_store.h"
#include <stdio.h>
#include <string.h>

const char *setup_validate_credentials(const char *ssid, const char *password) {
    if (!ssid || !ssid[0] || strlen(ssid) > 32) return "ssid";
    if (password && (strlen(password) > 64 || (password[0] && strlen(password) < 8))) return "password_format";
    return NULL;
}
static const char *host_agent_choice(const cJSON *request, const char *agent) {
    if (!strcmp(agent, "Hermes")) {
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "url"));
        return url && url[0] ? NULL : "url";
    }
    return !strcmp(agent, "OpenClaw") || !strcmp(agent, "Custom") ? NULL : "agent";
}
const char *setup_agent_choice(const cJSON *request, setup_agent_choice_t *choice) {
    memset(choice, 0, sizeof *choice);
    const char *agent = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "agent"));
    if (!agent) return "agent";
    choice->muse = !strcmp(agent, "Muse");
    if (!choice->muse) return host_agent_choice(request, agent);
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "sdk_token"));
    if (!token || !token[0]) return muse_store_saved_state() != MUSE_OFF ? NULL : "sdk_token";
    if (!muse_sdk_token_valid(token)) return "sdk_token";
    memcpy(choice->sdk_token, token, strlen(token) + 1); return NULL;
}
esp_err_t setup_agent_save(const setup_agent_choice_t *choice) {
    if (choice->muse && choice->sdk_token[0]) return muse_store_begin(choice->sdk_token);
    return muse_store_select(choice->muse);
}
cJSON *setup_read_request(httpd_req_t *request) {
    char body[512] = {0};
    if (request->content_len <= 0 || request->content_len >= (int)sizeof body) return NULL;
    int received = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) { muse_store_wipe(body, sizeof body); return NULL; }
        received += count;
    }
    cJSON *object = muse_json_parse(body, received);
    muse_store_wipe(body, sizeof body); return object;
}
