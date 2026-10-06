#include "link_discover.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "link_pin.h"
#include "settings.h"

#define DISCOVER_TRIES 3
#define DISCOVER_GAP_MS 1000
#define DISCOVER_REPLY_MAX 128

static const char *TAG = "discover";
static const char PROBE[] = "kubik-discover-v5";
static int s_sock = -1;
static int s_tries;
static uint32_t s_sent_ms;

void link_target_stop(void) {
    if (s_sock >= 0) close(s_sock);
    s_sock = -1;
}

static bool open_socket(void) {
    s_tries = 0;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) return false;
    // Bound to the station address, the broadcast leaves on the home network.
    struct sockaddr_in local = {.sin_family = AF_INET};
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) local.sin_addr.s_addr = ip.ip.addr;
    int on = 1;
    bool ok = setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof on) == 0 &&
        bind(s_sock, (struct sockaddr *)&local, sizeof local) == 0;
    if (!ok) link_target_stop();
    return ok;
}

// {"t":"kubik","v":5,"port":18790} -> the port, or 0 for anything else.
static int reply_port(const char *text, int len) {
    cJSON *j = cJSON_ParseWithLength(text, len);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "t"));
    cJSON *version = cJSON_GetObjectItemCaseSensitive(j, "v");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(j, "port");
    bool ok = type && !strcmp(type, "kubik") && cJSON_IsNumber(version) && version->valuedouble == 5 &&
        cJSON_IsNumber(port) && port->valuedouble >= 1 && port->valuedouble <= 65535 &&
        port->valuedouble == (int)port->valuedouble;
    int value = ok ? (int)port->valuedouble : 0;
    cJSON_Delete(j);
    return value;
}

// The plugin is wherever the reply came from.
static bool receive_reply(char *uri, size_t cap) {
    char text[DISCOVER_REPLY_MAX + 1];
    struct sockaddr_in from = {0};
    socklen_t from_len = sizeof from;
    int len = recvfrom(s_sock, text, sizeof text, MSG_DONTWAIT, (struct sockaddr *)&from, &from_len);
    if (len <= 0 || len > DISCOVER_REPLY_MAX || from.sin_family != AF_INET) return false;
    int port = reply_port(text, len);
    if (!port) return false;
    char ip[INET_ADDRSTRLEN];
    inet_ntoa_r(from.sin_addr, ip, sizeof ip);
    snprintf(uri, cap, "wss://%s:%d/kubik/v1", ip, port);
    ESP_LOGI(TAG, "agent host found at %s:%d", ip, port);
    return true;
}

static link_target_t discover(uint32_t now, char *uri, size_t cap) {
    if (s_sock < 0 && !open_socket()) return TARGET_NOT_FOUND;
    for (int i = 0; i < 4; i++) {
        if (receive_reply(uri, cap)) {
            link_target_stop();
            return TARGET_READY;
        }
    }
    if (s_tries && (uint32_t)(now - s_sent_ms) < DISCOVER_GAP_MS) return TARGET_WAITING;
    if (s_tries == DISCOVER_TRIES) {
        ESP_LOGW(TAG, "no agent host answered on this network");
        link_target_stop();
        return TARGET_NOT_FOUND;
    }
    struct sockaddr_in to = {
        .sin_family = AF_INET, .sin_port = htons(LINK_LAN_PORT), .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(s_sock, PROBE, sizeof PROBE - 1, 0, (struct sockaddr *)&to, sizeof to);
    s_tries++;
    s_sent_ms = now;
    return TARGET_WAITING;
}

// kubik://host[:port]: a port is a ':' after the host (past an IPv6 literal's ']').
static link_target_t fixed(const char *authority, char *uri, size_t cap) {
    const char *bracket = strrchr(authority, ']');
    bool has_port = strchr(bracket ? bracket : authority, ':') != NULL;
    int n = has_port ? snprintf(uri, cap, "wss://%s/kubik/v1", authority)
                     : snprintf(uri, cap, "wss://%s:%d/kubik/v1", authority, LINK_LAN_PORT);
    return n > 0 && (size_t)n < cap ? TARGET_READY : TARGET_NOT_FOUND;
}

link_target_t link_target(uint32_t now_ms, char *uri, size_t cap) {
    const char *url = g_settings.server_url;
    switch (link_server_mode(url)) {
    case SERVER_LAN_DISCOVER: return discover(now_ms, uri, cap);
    case SERVER_LAN_FIXED: return fixed(url + 8, uri, cap);
    default:
        snprintf(uri, cap, "%s", url);
        return TARGET_READY;
    }
}
