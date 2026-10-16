#include "setup_request.h"
#include "muse_json.h"
#include "muse_store.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#ifdef ESP_PLATFORM
#include "lwip/ip_addr.h"
#else
#include <arpa/inet.h>
#endif

const char *setup_validate_credentials(const char *ssid, const char *password) {
    if (!ssid || !ssid[0] || strlen(ssid) > 32) return "ssid";
    if (password && (strlen(password) > 64 || (password[0] && strlen(password) < 8))) return "password_format";
    return NULL;
}
#ifdef ESP_PLATFORM
static bool local_ip(const char *host) {
    ip_addr_t address;
    if (!ipaddr_aton(host, &address)) return false;
    if (ip_addr_isany(&address) || ip_addr_isloopback(&address)) return true;
#if LWIP_IPV4 && LWIP_IPV6
    if (IP_IS_V6(&address)) {
        const ip6_addr_t *v6 = ip_2_ip6(&address);
        if (!v6->addr[0] && !v6->addr[1] && v6->addr[2] == PP_HTONL(0x0000ffffUL)) {
            uint32_t v4 = lwip_ntohl(v6->addr[3]);
            return !v4 || (v4 >> 24) == 127;
        }
    }
#endif
    return false;
}
#else
static bool local_ip(const char *host) {
    struct in_addr v4;
    if (inet_aton(host, &v4)) return v4.s_addr == 0 || (ntohl(v4.s_addr) >> 24) == 127;
    unsigned char v6[16];
    if (inet_pton(AF_INET6, host, v6) != 1) return false;
    static const unsigned char zero[15] = {0};
    static const unsigned char mapped[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
    if (!memcmp(v6, zero, 15)) return v6[15] <= 1;
    return !memcmp(v6, mapped, 12) && (v6[12] == 127 || !memcmp(v6 + 12, zero, 4));
}
#endif
static bool local_host(const char *host) {
    size_t length = strlen(host);
    if (!strcasecmp(host, "localhost") ||
        (length > 10 && !strcasecmp(host + length - 10, ".localhost"))) return true;
    return local_ip(host);
}
static bool hermes_port_valid(const char *text) {
    unsigned port = 0;
    if (!text || !text[0]) return false;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') return false;
        port = port * 10 + (unsigned)(*text - '0');
        if (port > 65535) return false;
    }
    return port >= 1024;
}
static bool hermes_address_valid(const char *url) {
    if (!url || strnlen(url, 128) >= 128 || strncmp(url, "kubik://", 8)) return false;
    const char *start = url + 8, *port;
    bool bracketed = *start == '[';
    if (bracketed) {
        start++;
        const char *close = strchr(start, ']');
        if (!close || close[1] != ':') return false;
        port = close + 2;
    } else {
        port = strchr(start, ':');
        if (!port || strchr(port + 1, ':')) return false;
        port++;
    }
    size_t length = (size_t)((bracketed ? port - 2 : port - 1) - start);
    if (!length || !hermes_port_valid(port)) return false;
    char host[128];
    memcpy(host, start, length); host[length] = 0;
    return !local_host(host);
}
static const char *host_agent_choice(const cJSON *request, const char *agent, setup_agent_choice_t *choice) {
    if (!strcmp(agent, "Hermes")) {
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "url"));
        if (!hermes_address_valid(url)) return "server_format";
        choice->host_agent = SETTINGS_AGENT_HERMES;
        return NULL;
    }
    if (!strcmp(agent, "Custom")) { choice->host_agent = SETTINGS_AGENT_CUSTOM; return NULL; }
    return !strcmp(agent, "OpenClaw") ? NULL : "agent";
}
const char *setup_agent_choice(const cJSON *request, setup_agent_choice_t *choice) {
    memset(choice, 0, sizeof *choice);
    const char *agent = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "agent"));
    if (!agent) return "agent";
    choice->muse = !strcmp(agent, "Muse");
    if (!choice->muse) return host_agent_choice(request, agent, choice);
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "sdk_token"));
    if (!token || !token[0]) return muse_store_saved_state() != MUSE_OFF ? NULL : "sdk_token";
    if (!muse_sdk_token_valid(token)) return "sdk_token";
    memcpy(choice->sdk_token, token, strlen(token) + 1); return NULL;
}
esp_err_t setup_connection_save(const setup_agent_choice_t *choice,
                                const char *ssid, const char *password, const char *url) {
    if (!choice) return ESP_ERR_INVALID_ARG;
    const char *sdk_token = choice->muse && choice->sdk_token[0] ? choice->sdk_token : NULL;
    return settings_save_setup(ssid, password, url, choice->muse ? SETTINGS_AGENT_MUSE : choice->host_agent, sdk_token);
}
esp_err_t setup_host_connection_save(const cJSON *request,
                                      const char *ssid, const char *password, const char *url) {
    const cJSON *agent = cJSON_GetObjectItemCaseSensitive(request, "agent");
    if (!agent) {
        if (g_settings.host_agent == SETTINGS_AGENT_HERMES && !hermes_address_valid(url))
            return ESP_ERR_INVALID_ARG;
        return settings_save_connection(ssid, password, url);
    }
    setup_agent_choice_t choice;
    if (setup_agent_choice(request, &choice) || choice.muse) return ESP_ERR_INVALID_ARG;
    if (choice.host_agent != g_settings.host_agent &&
        !cJSON_GetObjectItemCaseSensitive(request, "url")) url = "";
    return setup_connection_save(&choice, ssid, password, url);
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
