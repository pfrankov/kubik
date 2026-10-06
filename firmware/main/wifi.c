#include "wifi.h"

#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "settings.h"
#include "power_network.h"

static const char *TAG = "wifi";
#define RECOVERY_MS 60000
#define PROFILE_RETRIES 3

static bool s_inited;
static atomic_bool s_ap_on;
static atomic_bool s_started, s_radio_blocked;
static SemaphoreHandle_t s_driver_mutex;
static atomic_bool s_connected, s_have_sta, s_scanning;
static esp_timer_handle_t s_reconnect;
static esp_netif_t *s_ap;
static atomic_int s_retry, s_attempts, s_clients;
static atomic_uchar s_reason;
static _Atomic TickType_t s_disconnected_at;
static atomic_bool s_sta_armed, s_trial;
static int s_profile_index;
static char s_station_ssid[33];
static atomic_bool s_polling, s_poll_prepare;
static atomic_uint s_poll_link_attempts;
static _Atomic TickType_t s_poll_started;
static void set_sta(const char *ssid, const char *password);

static void reconnect(void *arg) {
    if (!s_have_sta || s_scanning || s_radio_blocked) return;
    xSemaphoreTake(s_driver_mutex, portMAX_DELAY);
    if (s_have_sta && !s_scanning && !s_radio_blocked) esp_wifi_connect();
    xSemaphoreGive(s_driver_mutex);
}

static int profile_count(void) { return g_settings.wifi_profile_count; }

static bool select_profile_config(int index) {
    if (index < 0 || index >= profile_count()) return false;
    s_profile_index = index;
    const wifi_profile_t *profile = &g_settings.wifi_profiles[index];
    snprintf(g_settings.wifi_ssid, sizeof g_settings.wifi_ssid, "%s", profile->ssid);
    snprintf(g_settings.wifi_pass, sizeof g_settings.wifi_pass, "%s", profile->password);
    return true;
}

static void select_profile(int index) {
    if (!select_profile_config(index)) { s_have_sta = false; return; }
    set_sta(g_settings.wifi_ssid, g_settings.wifi_pass);
}

static void rotate_profile(void) {
    int count = profile_count();
    if (s_trial || count < 2) return;
    select_profile((s_profile_index + 1) % count);
    s_attempts = 0;
    s_retry = 0;
    ESP_LOGI(TAG, "trying saved Wi-Fi profile %d/%d", s_profile_index + 1, count);
}

static bool rotate_after_failures(void) {
    if (s_trial || s_attempts < PROFILE_RETRIES || profile_count() < 2) return false;
    rotate_profile();
    return true;
}

static void schedule_retry(bool changed_profile) {
    if (!changed_profile && s_retry < 5) s_retry++;
    esp_timer_stop(s_reconnect);
    if (s_have_sta) {
        uint64_t delay = changed_profile ? 300000 : s_retry < 5 ? 1000000 : 30000000;
        esp_timer_start_once(s_reconnect, delay);
    }
}

static void handle_disconnect(const wifi_event_sta_disconnected_t *event) {
    if (s_radio_blocked || s_scanning || !s_have_sta) { s_connected = false; return; }
    xSemaphoreTake(s_driver_mutex, portMAX_DELAY);
    if (!s_radio_blocked) {
        if (s_connected) s_disconnected_at = xTaskGetTickCount();
        s_connected = false;
        s_reason = event ? event->reason : 0;
        if (!event || event->reason != WIFI_REASON_ASSOC_LEAVE) s_attempts++;
        schedule_retry(rotate_after_failures());
    }
    xSemaphoreGive(s_driver_mutex);
}

static void handle_wifi_event(int32_t id, void *data) {
    switch (id) {
    case WIFI_EVENT_STA_START:
        reconnect(NULL);
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        handle_disconnect(data);
        break;
    case WIFI_EVENT_AP_STACONNECTED:
        s_clients++;
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        if (s_clients > 0) s_clients--;
        break;
    default:
        break;
    }
}

static void handle_got_ip(const ip_event_got_ip_t *event) {
    xSemaphoreTake(s_driver_mutex, portMAX_DELAY);
    wifi_ap_record_t ap;
    bool current = s_have_sta && esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
        !strncmp((const char *)ap.ssid, s_station_ssid, sizeof ap.ssid);
    if (!s_radio_blocked && current) {
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        s_attempts = 0;
        esp_timer_stop(s_reconnect);
    }
    xSemaphoreGive(s_driver_mutex);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) handle_wifi_event(id, data);
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) handle_got_ip(data);
}

void wifi_init(void) {
    if (s_inited) return;
    s_driver_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_driver_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    s_inited = true;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(sta ? ESP_OK : ESP_ERR_NO_MEM);
    // English DHCP name with the stable hardware-derived device suffix.
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta, g_device_id));
    esp_timer_create_args_t timer = {.callback = reconnect, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_reconnect));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_sntp_config_t time_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.cloudflare.com");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&time_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
}

bool wifi_sta_connected(void) { return s_connected; }
static void network_addresses(esp_netif_t *sta, device_status_t *status, const esp_netif_ip_info_t *ip) {
    snprintf(status->ip, sizeof status->ip, IPSTR, IP2STR(&ip->ip));
    snprintf(status->gateway, sizeof status->gateway, IPSTR, IP2STR(&ip->gw));
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4)
        snprintf(status->dns, sizeof status->dns, IPSTR, IP2STR(&dns.ip.u_addr.ip4));
}
void wifi_status(device_status_t *status) {
    status->ip[0] = status->gateway[0] = status->dns[0] = 0;
    status->mac[0] = status->hostname[0] = 0; status->connected = false;
    status->radio = s_started;
    snprintf(status->ssid, sizeof status->ssid, "%s", g_settings.wifi_ssid);
    if (!s_inited) return;
    xSemaphoreTake(s_driver_mutex, portMAX_DELAY);
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    const char *hostname = NULL;
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK)
        snprintf(status->mac, sizeof status->mac, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (sta && esp_netif_get_hostname(sta, &hostname) == ESP_OK && hostname)
        snprintf(status->hostname, sizeof status->hostname, "%s", hostname);
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip;
    status->connected = sta && s_connected && esp_netif_is_netif_up(sta) &&
        esp_wifi_sta_get_ap_info(&ap) == ESP_OK && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr;
    if (status->connected) {
        snprintf(status->ssid, sizeof status->ssid, "%.*s", 32, (const char *)ap.ssid);
        network_addresses(sta, status, &ip);
    }
    xSemaphoreGive(s_driver_mutex);
}
bool wifi_radio_started(void) { return s_started; }
bool wifi_polling(void) { return s_polling; }
bool wifi_poll_join_expired(void) {
    return s_polling && !s_connected && (TickType_t)(xTaskGetTickCount() - s_poll_started) >= pdMS_TO_TICKS(POWER_NETWORK_JOIN_MS);
}
bool wifi_link_attempt_allowed(void) { return !s_polling || s_poll_link_attempts < 3; }
void wifi_link_attempt_started(void) { if (s_polling) s_poll_link_attempts++; }
bool wifi_time_ready(void) { return time(NULL) >= 1704067200; }
bool wifi_take_recovery_request(void) {
    TickType_t now = xTaskGetTickCount();
    if (s_radio_blocked || !s_sta_armed || s_connected ||
        (TickType_t)(now - s_disconnected_at) < pdMS_TO_TICKS(RECOVERY_MS)) return false;
    s_disconnected_at = now;
    return true;
}

static atomic_bool s_fast, s_doze;
// A conversation: no power save. Dozing (screen off): wake for every third beacon only. Else every DTIM.
static wifi_ps_type_t ps_mode(void) { return s_fast ? WIFI_PS_NONE : s_doze ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM; }
static void apply_ps(void) {
    if (!s_driver_mutex) return;
    xSemaphoreTake(s_driver_mutex, portMAX_DELAY);
    if (s_started && !s_radio_blocked) esp_wifi_set_ps(ps_mode());
    xSemaphoreGive(s_driver_mutex);
}
void wifi_set_fast(bool fast) {
    if (fast == s_fast) return;
    s_fast = fast;
    apply_ps();
}
void wifi_set_doze(bool doze) {
    if (doze == s_doze) return;
    s_doze = doze;
    apply_ps();
}

static void start_locked(void) {
    if (s_started) return;
    esp_wifi_set_ps(ps_mode());
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;
}

static void set_sta(const char *ssid, const char *password) {
    wifi_config_t wc = {0};
    // Fields may be completely full (no terminator), as the Wi-Fi driver allows.
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, password, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.listen_interval = 3;  // beacons slept through in WIFI_PS_MAX_MODEM (dozing)
    snprintf(s_station_ssid, sizeof s_station_ssid, "%s", ssid);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_have_sta = ssid[0] != 0;
}

void wifi_radio_stop(void) {
    s_radio_blocked = true;  // close admission before waiting for an in-flight start
    if (!s_driver_mutex) return;
    xSemaphoreTake(s_driver_mutex, portMAX_DELAY);
    s_polling = s_poll_prepare = false;
    s_sta_armed = false;
    s_have_sta = false;
    s_connected = false;
    s_attempts = s_retry = 0;
    s_disconnected_at = xTaskGetTickCount();
    esp_timer_stop(s_reconnect);
    if (s_started) {
        int err = esp_wifi_stop();
        if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_STARTED) s_started = false;
        else ESP_LOGW(TAG, "radio stop failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_driver_mutex);
}

void wifi_radio_resume(bool polling) {
    if (s_driver_mutex) xSemaphoreTake(s_driver_mutex, portMAX_DELAY);
    s_radio_blocked = false;
    s_polling = s_poll_prepare = polling;
    s_poll_link_attempts = 0;
    s_poll_started = xTaskGetTickCount();
    if (s_driver_mutex) xSemaphoreGive(s_driver_mutex);
}

static int visible_saved_profile(int selected) {
    wifi_net_t nearby[24];
    int count = wifi_scan(nearby, 24);
    int strongest = -1;
    for (int i = 0; i < count; i++) {
        int profile = settings_wifi_find(nearby[i].ssid);
        if (profile < 0) continue;
        if (profile == selected) return selected;
        if (strongest < 0) strongest = profile;
    }
    // A hidden saved SSID still uses directed station association.
    return strongest >= 0 ? strongest : selected;
}

static bool station_start_allowed(void) {
    return !s_radio_blocked && !s_trial && !s_sta_armed && profile_count() && !wifi_poll_join_expired();
}

void wifi_start_sta(void) {
    if (!station_start_allowed()) return;
    wifi_init();
    int index = settings_wifi_find(g_settings.wifi_ssid);
    if (index < 0) index = 0;
    if (s_poll_prepare) {
        s_poll_prepare = false;
        index = visible_saved_profile(index);
        if (s_radio_blocked || wifi_poll_join_expired()) return;
    }
    xSemaphoreTake(s_driver_mutex, portMAX_DELAY);
    if (!station_start_allowed()) {
        xSemaphoreGive(s_driver_mutex);
        return;
    }
    esp_wifi_set_mode(s_ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    select_profile(index);
    s_disconnected_at = xTaskGetTickCount();
    s_sta_armed = true;
    if (s_started) esp_wifi_connect();
    start_locked();
    xSemaphoreGive(s_driver_mutex);
    ESP_LOGI(TAG, "joining '%s'", g_settings.wifi_ssid);
}

void wifi_try(const char *ssid, const char *password) {
    wifi_init();
    xSemaphoreTake(s_driver_mutex, portMAX_DELAY);
    s_radio_blocked = false;
    if (!ssid) {
        s_trial = false;
        int index = settings_wifi_find(g_settings.wifi_ssid);
        if (index < 0) index = 0;
        bool saved = select_profile_config(index);
        ssid = saved ? g_settings.wifi_ssid : "";
        password = saved ? g_settings.wifi_pass : "";
    } else {
        s_trial = true;
    }
    esp_timer_stop(s_reconnect);
    bool was_armed = s_have_sta || s_connected;
    s_have_sta = false;  // our own disconnect must not schedule a retry of the old network
    if (was_armed) esp_wifi_disconnect();
    s_connected = false;
    set_sta(ssid, password);
    s_sta_armed = !s_trial && s_have_sta;
    s_disconnected_at = xTaskGetTickCount();
    s_retry = 0;
    s_attempts = 0;
    s_reason = 0;
    start_locked();
    if (s_have_sta) {
        ESP_LOGI(TAG, "trying '%s'", ssid);
        esp_timer_start_once(s_reconnect, 300000);
    }
    xSemaphoreGive(s_driver_mutex);
}

int wifi_attempts(void) { return s_attempts; }
uint8_t wifi_last_reason(void) { return s_reason; }

void wifi_ap_start(const char *ssid, const char *password) {
    wifi_init();
    xSemaphoreTake(s_driver_mutex, portMAX_DELAY);
    s_radio_blocked = false;
    if (!s_ap) {
        s_ap = esp_netif_create_default_wifi_ap();
        ESP_ERROR_CHECK(s_ap ? ESP_OK : ESP_ERR_NO_MEM);
        // RFC 8910: phones that support it open the setup page without probing.
        static const char uri[] = "http://192.168.4.1/";
        esp_netif_dhcps_stop(s_ap);
        esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, (void *)uri, sizeof(uri) - 1);
        esp_netif_dhcps_start(s_ap);
    }
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    strncpy((char *)ap.ap.password, password, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 6;
    ap.ap.pmf_cfg.required = false;
    s_clients = 0;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_ap_on = true;
    start_locked();
    xSemaphoreGive(s_driver_mutex);
    ESP_LOGI(TAG, "setup access point '%s' up", ssid);
}

void wifi_ap_stop(void) {
    if (!s_ap_on) return;
    xSemaphoreTake(s_driver_mutex, portMAX_DELAY);
    s_ap_on = false;
    s_clients = 0;
    esp_wifi_set_mode(WIFI_MODE_STA);
    xSemaphoreGive(s_driver_mutex);
    ESP_LOGI(TAG, "setup access point down");
}

int wifi_ap_clients(void) { return s_clients; }
bool wifi_ap_active(void) { return s_ap_on; }

static int copy_scan(wifi_net_t *out, int cap) {
    uint16_t count = 24;
    static wifi_ap_record_t recs[24];
    if (esp_wifi_scan_get_ap_records(&count, recs) != ESP_OK) return 0;
    int n = 0;
    for (int i = 0; i < count; i++) {  // records arrive strongest first
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int k = 0; k < n && !dup; k++) dup = !strcmp(out[k].ssid, ssid);
        if (dup || n >= cap) continue;
        strncpy(out[n].ssid, ssid, sizeof(out[n].ssid) - 1);
        out[n].ssid[sizeof(out[n].ssid) - 1] = 0;
        out[n].rssi = recs[i].rssi;
        out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        n++;
    }
    return n;
}

static int scan_locked(wifi_net_t *out, int cap) {
    // A pending connect attempt makes the driver refuse to scan: pause retries.
    s_scanning = true;
    esp_timer_stop(s_reconnect);
    if (!s_connected && s_have_sta) esp_wifi_disconnect();
    wifi_scan_config_t sc = {.show_hidden = false, .scan_time.active = {.min = 80, .max = 160}};
    int n = 0;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        n = copy_scan(out, cap);
    } else {
        esp_wifi_clear_ap_list();
    }
    s_scanning = false;
    if (!s_radio_blocked && !s_connected && s_have_sta) esp_timer_start_once(s_reconnect, 500000);
    return n;
}

int wifi_scan(wifi_net_t *out, int cap) {
    wifi_init();
    xSemaphoreTake(s_driver_mutex, portMAX_DELAY);
    int count = 0;
    if (!s_radio_blocked) {
        esp_wifi_set_mode(s_ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
        start_locked();
        count = scan_locked(out, cap);
    }
    xSemaphoreGive(s_driver_mutex);
    return count;
}
