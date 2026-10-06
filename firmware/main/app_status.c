#include "app_status.h"
#include "app_internal.h"
#include "app_lab.h"
#include "wifi.h"
#include "muse_backend.h"
#include "muse_noise.h"
#include "version.h"
#include <stdio.h>
#include <string.h>

static void snapshot(device_status_t *status) {
    wifi_status(status);
    status->agent = s_online;
    snprintf(status->name, sizeof status->name, "%s", g_settings.name);
    snprintf(status->firmware, sizeof status->firmware, "%s", KUBIK_FW_VERSION);
    snprintf(status->route, sizeof status->route, "%s", link_via());
    snprintf(status->url, sizeof status->url, "%s", muse_backend_selected() ? muse_noise_endpoint() : g_settings.server_url);
    if (!muse_backend_selected() && !strcmp(status->route, "wifi"))
        link_endpoint(status->url, sizeof status->url);
    // Provider secrets are never part of the status screen.
    char *query = strpbrk(status->url, "?#");
    if (query) *query = 0;
}
void app_status_refresh(void) {
    static int64_t next;
    if (!g_face.status.open || app_lab_active() || now_ms() < next) return;
    next = now_ms() + 1000;
    device_status_t status = {0}; snapshot(&status);
    face_lock();
    status.open = g_face.status.open; status.page = g_face.status.page;
    status.drag_x = g_face.status.drag_x; status.drag_y = g_face.status.drag_y;
    status.dragged = g_face.status.dragged;
    g_face.status = status;
    face_unlock();
}
void app_status_open(void) {
    device_status_t status = {.open = true}; snapshot(&status);
    face_lock(); g_face.status = status; face_unlock();
}
void app_status_info(cJSON *info) {
    device_status_t status = {0}; snapshot(&status);
    cJSON_AddStringToObject(info, "device_name", status.name);
    cJSON_AddStringToObject(info, "wifi_ip", status.ip);
    cJSON_AddStringToObject(info, "wifi_gateway", status.gateway);
    cJSON_AddStringToObject(info, "wifi_dns", status.dns);
    cJSON_AddStringToObject(info, "wifi_mac", status.mac);
    cJSON_AddStringToObject(info, "dhcp_hostname", status.hostname);
    face_lock();
    cJSON_AddBoolToObject(info, "status_open", g_face.status.open);
    cJSON_AddNumberToObject(info, "status_page", g_face.status.page);
    face_unlock();
}
