#!/usr/bin/env python3
"""Exercise the real station recovery code with deterministic clock/radio stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = "\n".join(line for line in (root / "firmware/main/wifi.c").read_text().splitlines()
                   if not line.startswith("#include"))
mock = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "power_network.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "wifi.h"
typedef uint32_t TickType_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
static int driver_locked;
static pthread_mutex_t driver_mutex = PTHREAD_MUTEX_INITIALIZER;
static void (*before_take)(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &driver_mutex; }
int xSemaphoreTake(SemaphoreHandle_t lock, uint32_t timeout) {
    if (before_take) { void (*hook)(void) = before_take; before_take = NULL; hook(); }
    assert(pthread_mutex_lock(lock) == 0); assert(!driver_locked); driver_locked = 1; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t lock) { assert(driver_locked); driver_locked = 0; assert(pthread_mutex_unlock(lock) == 0); return 1; }
typedef int esp_event_base_t;
typedef void *esp_timer_handle_t;
typedef int esp_netif_t;
typedef struct { int unused; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(s) ((esp_sntp_config_t){0})
int esp_netif_sntp_init(esp_sntp_config_t *cfg) { return 0; }
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
typedef struct { int nvs_enable; } wifi_init_config_t;
typedef struct {
    struct { uint8_t ssid[32], password[64]; int listen_interval;
             struct { int authmode; } threshold; struct { bool capable; } pmf_cfg; } sta;
    struct { uint8_t ssid[32], password[64]; uint8_t ssid_len, channel, max_connection; int authmode;
             struct { bool required; } pmf_cfg; } ap;
} wifi_config_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { struct { esp_ip4_addr_t ip; } ip_info; } ip_event_got_ip_t;
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int)((a)->addr>>24), (int)(((a)->addr>>16)&255), (int)(((a)->addr>>8)&255), (int)((a)->addr&255)
typedef struct { uint8_t reason; } wifi_event_sta_disconnected_t;
#define WIFI_REASON_AUTH_FAIL 202
#define WIFI_REASON_ASSOC_LEAVE 8
#define WIFI_REASON_NO_AP_FOUND 201
typedef struct { bool show_hidden; struct { struct { int min, max; } active; } scan_time; } wifi_scan_config_t;
typedef struct { uint8_t ssid[33]; int8_t rssi; int authmode; } wifi_ap_record_t;
typedef struct { char ssid[33], password[65]; } wifi_profile_t;
static struct { wifi_profile_t wifi_profiles[8]; uint8_t wifi_profile_count; char wifi_ssid[33], wifi_pass[65]; } g_settings;
static int settings_wifi_find(const char *ssid) {
    for (int i = 0; i < g_settings.wifi_profile_count; i++)
        if (!strcmp(g_settings.wifi_profiles[i].ssid, ssid)) return i;
    return -1;
}
static char g_device_id[20] = "kubik-abcdef";
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -1
#define ESP_ERR_WIFI_NOT_STARTED 2
#define ESP_ERROR_CHECK(x) assert((x)==0)
#define ESP_LOGI(...) test_log()
static void test_log(void);
#define ESP_LOGW(...) ((void)0)
static const char *esp_err_to_name(int err) { (void)err; return "error"; }
#define WIFI_EVENT 1
#define IP_EVENT 2
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define WIFI_EVENT_AP_STACONNECTED 4
#define WIFI_EVENT_AP_STADISCONNECTED 5
#define IP_EVENT_STA_GOT_IP 3
#define ESP_EVENT_ANY_ID 99
#define WIFI_MODE_STA 1
#define WIFI_MODE_APSTA 3
#define WIFI_STORAGE_RAM 1
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA_PSK 1
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_PS_NONE 0
#define WIFI_PS_MIN_MODEM 1
#define WIFI_PS_MAX_MODEM 2
typedef int wifi_ps_type_t;
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1
#define ESP_NETIF_OP_SET 1
#define ESP_NETIF_CAPTIVEPORTAL_URI 114
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){1})
#define pdMS_TO_TICKS(x) (x)
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data);
static void reconnect(void *arg);
static TickType_t clock_ms;
static int starts, stops, connects, timers, disconnects, ap_netifs, mode = -1, ps_mode_seen = -1;
static uint64_t timer_us;
static bool associated;
static bool timer_armed, sync_leave = true, scan_fails;
static char sta_ssid[33], connect_ssid[33], hostname[32], portal_uri[64], ap_ssid[33];
static wifi_config_t ap_cfg;
TickType_t xTaskGetTickCount(void) { return clock_ms; }
int esp_wifi_connect(void) { associated = true; connects++; strcpy(connect_ssid, sta_ssid); return 0; }
int esp_wifi_disconnect(void) {
    // Our own disconnect: the driver reports ASSOC_LEAVE (synchronously here, or later).
    disconnects++; associated = false;
    if (sync_leave) { wifi_event_sta_disconnected_t e = {WIFI_REASON_ASSOC_LEAVE}; on_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &e); }
    return 0;
}
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *record) {
    if (!associated) return ESP_FAIL;
    memset(record, 0, sizeof *record);
    snprintf((char *)record->ssid, sizeof record->ssid, "%s", connect_ssid);
    return ESP_OK;
}
int esp_timer_stop(esp_timer_handle_t t) { timer_armed = false; return 0; }
int esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { timers++; timer_us = us; timer_armed = true; return 0; }
int esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) { assert(a->callback == reconnect); *h = (void *)1; return 0; }
static void fire_timer(void) { if (timer_armed) { timer_armed = false; reconnect(NULL); } }
int esp_netif_init(void) { return 0; }
int esp_event_loop_create_default(void) { return 0; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { static int n; return &n; }
esp_netif_t *esp_netif_create_default_wifi_ap(void) { static int n; ap_netifs++; return &n; }
int esp_netif_set_hostname(esp_netif_t *n, const char *name) { snprintf(hostname, sizeof hostname, "%s", name); return 0; }
typedef struct { esp_ip4_addr_t ip, gw, netmask; } esp_netif_ip_info_t;
typedef struct { struct { int type; union { esp_ip4_addr_t ip4; } u_addr; } ip; } esp_netif_dns_info_t;
#define ESP_NETIF_DNS_MAIN 0
#define ESP_IPADDR_TYPE_V4 0
static bool status_up = true, status_dns = true;
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { return esp_netif_create_default_wifi_sta(); }
bool esp_netif_is_netif_up(esp_netif_t *n) { return status_up; }
int esp_netif_get_hostname(esp_netif_t *n, const char **out) { *out = hostname; return ESP_OK; }
int esp_wifi_get_mac(int iface, uint8_t *out) { memcpy(out, (uint8_t[]){2,0,0,18,52,86}, 6); return ESP_OK; }
int esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *out) {
    *out = (esp_netif_ip_info_t){.ip.addr=0x0a011e35, .gw.addr=0x0a011e01}; return ESP_OK;
}
int esp_netif_get_dns_info(esp_netif_t *n, int kind, esp_netif_dns_info_t *out) {
    out->ip.type = ESP_IPADDR_TYPE_V4; out->ip.u_addr.ip4.addr=0x01010101; return status_dns ? ESP_OK : ESP_FAIL;
}
int esp_netif_dhcps_stop(esp_netif_t *n) { return 0; }
int esp_netif_dhcps_start(esp_netif_t *n) { return 0; }
int esp_netif_dhcps_option(esp_netif_t *n, int op, int id, void *v, uint32_t len) {
    assert(op == ESP_NETIF_OP_SET && id == ESP_NETIF_CAPTIVEPORTAL_URI && len < sizeof portal_uri);
    memcpy(portal_uri, v, len); portal_uri[len] = 0; return 0;
}
int esp_wifi_init(wifi_init_config_t *cfg) { assert(!cfg->nvs_enable); return 0; }
int esp_wifi_set_storage(int s) { assert(s==WIFI_STORAGE_RAM); return 0; }
int esp_event_handler_register(int a,int b,void (*f)(void *,int,int32_t,void *),void *arg) { return 0; }
int esp_wifi_set_mode(int v) { mode = v; return 0; }
int esp_wifi_set_config(int i,wifi_config_t *cfg) {
    if (i == WIFI_IF_STA) { memcpy(sta_ssid, cfg->sta.ssid, 32); sta_ssid[32] = 0; }
    else { assert(i == WIFI_IF_AP); ap_cfg = *cfg; memcpy(ap_ssid, cfg->ap.ssid, 32); ap_ssid[32] = 0; }
    return 0;
}
static atomic_bool s_radio_blocked; // tentative; defined by wifi.c
static bool stop_during_start, stop_during_scan, stop_during_ip, stop_during_ps;
static pthread_t stopping_thread;
static void *stop_thread(void *arg) { wifi_radio_stop(); return NULL; }
static void stop_inside_driver_transaction(void) {
    assert(pthread_create(&stopping_thread, NULL, stop_thread, NULL) == 0);
    while (!s_radio_blocked) sched_yield();
}
int esp_wifi_set_ps(int v) {
    assert(driver_locked);
    if (stop_during_ps) { stop_during_ps = false; stop_inside_driver_transaction(); }
    ps_mode_seen = v; return 0;
}
static void test_log(void) {
    if (stop_during_ip) { stop_during_ip = false; stop_inside_driver_transaction(); }
}
int esp_wifi_start(void) {
    assert(driver_locked); starts++;
    if (stop_during_start) {
        stop_during_start = false;
        stop_inside_driver_transaction(); // stop closes gate while this start holds the driver lock
    }
    return 0;
}
int esp_wifi_stop(void) { assert(driver_locked); stops++; return 0; }
static wifi_ap_record_t scan_recs[6] = {
    {"home", -40, WIFI_AUTH_WPA2_PSK}, {"", -45, WIFI_AUTH_OPEN}, {"cafe", -50, WIFI_AUTH_OPEN},
    {"home", -60, WIFI_AUTH_WPA2_PSK}, {"office", -70, WIFI_AUTH_WPA_PSK}, {"far", -90, WIFI_AUTH_OPEN}};
static atomic_bool s_scanning; // tentative; defined by wifi.c
static bool scanning_seen;
int esp_wifi_scan_start(wifi_scan_config_t *c, bool block) {
    // A retry timer firing during the blocking scan must not start a connect.
    assert(driver_locked);
    if (stop_during_scan) { stop_during_scan = false; stop_inside_driver_transaction(); }
    int c0 = connects; assert(block); scanning_seen = s_scanning; fire_timer(); assert(connects == c0);
    return scan_fails ? ESP_FAIL : ESP_OK;
}
int esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *r) { assert(*n >= 6); memcpy(r, scan_recs, sizeof scan_recs); *n = 6; return 0; }
int esp_wifi_clear_ap_list(void) { return 0; }
'''
test = r'''
static void stop_before_driver_start(void) { wifi_radio_stop(); }
static void stale_start_test(void) {
    wifi_radio_stop();
    wifi_radio_resume(false);
    int previous = starts;
    before_take = stop_before_driver_start;
    wifi_start_sta();
    assert(starts == previous && !wifi_radio_started());
    fire_timer(); assert(!wifi_radio_started());
    on_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    assert(!wifi_radio_started());
    wifi_radio_resume(false); wifi_start_sta(); assert(wifi_radio_started());
    wifi_radio_stop(); assert(!wifi_radio_started());
}
static void in_flight_start_test(void) {
    wifi_radio_stop(); wifi_radio_resume(false);
    int previous_stops = stops;
    stop_during_start = true; wifi_start_sta();
    assert(pthread_join(stopping_thread, NULL) == 0);
    assert(!wifi_radio_started() && stops == previous_stops + 1);
    int previous_starts = starts; wifi_start_sta(); assert(starts == previous_starts);
}
static void scan_and_ip_race_test(void) {
    wifi_radio_stop(); wifi_radio_resume(false); wifi_start_sta();
    int previous_stops = stops;
    wifi_net_t networks[3]; stop_during_scan = true; wifi_scan(networks, 3);
    assert(pthread_join(stopping_thread, NULL) == 0);
    assert(!wifi_radio_started() && stops == previous_stops + 1);
    wifi_radio_resume(false); wifi_start_sta();
    on_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL); // station is actually associated before GOT_IP
    stop_during_ip = true; ip_event_got_ip_t ip = {0}; handle_got_ip(&ip);
    assert(pthread_join(stopping_thread, NULL) == 0);
    assert(!wifi_radio_started() && !wifi_sta_connected());
}
static void power_save_race_test(void) {
    wifi_radio_resume(false); wifi_start_sta();
    stop_during_ps = true; wifi_set_fast(!s_fast);
    assert(pthread_join(stopping_thread, NULL) == 0);
    assert(!wifi_radio_started());
    wifi_radio_resume(false); wifi_start_sta();
    int previous = ps_mode_seen;
    before_take = stop_before_driver_start;
    wifi_set_doze(!s_doze);
    assert(!wifi_radio_started() && ps_mode_seen == previous);
}
static void disconnected(uint8_t reason) { wifi_event_sta_disconnected_t e = {reason}; on_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&e); }
int main(void) {
    wifi_start_sta(); assert(starts==0 && !wifi_take_recovery_request());
    strcpy(g_settings.wifi_profiles[0].ssid,"saved-network");
    strcpy(g_settings.wifi_profiles[0].password,"saved-password");
    g_settings.wifi_profile_count=1;
    strcpy(g_settings.wifi_ssid,"saved-network"); wifi_start_sta(); wifi_start_sta(); assert(starts==1);
    assert(!strcmp(hostname,"kubik-abcdef") && mode==WIFI_MODE_STA && !strcmp(sta_ssid,"saved-network"));
    assert(ps_mode_seen == WIFI_PS_MIN_MODEM);
    wifi_set_doze(true); assert(ps_mode_seen == WIFI_PS_MAX_MODEM);
    wifi_set_fast(true); assert(ps_mode_seen == WIFI_PS_NONE);
    wifi_set_fast(false); assert(ps_mode_seen == WIFI_PS_MAX_MODEM);
    wifi_set_doze(false); assert(ps_mode_seen == WIFI_PS_MIN_MODEM);
    on_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_START,NULL); assert(connects==1 && !strcmp(connect_ssid,"saved-network"));
    // Saved network lost for 60 s -> one recovery request, then at most once per 60 s.
    clock_ms=59999; assert(!wifi_take_recovery_request());
    clock_ms=60000; assert(wifi_take_recovery_request()); assert(!wifi_take_recovery_request());
    clock_ms=119999; assert(!wifi_take_recovery_request());
    clock_ms=120000; assert(wifi_take_recovery_request()); assert(!wifi_take_recovery_request());
    ip_event_got_ip_t ip={0}; on_event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    clock_ms=200000; assert(wifi_sta_connected() && !wifi_take_recovery_request());
    device_status_t status = {0}; wifi_status(&status);
    assert(status.connected && !strcmp(status.ip,"10.1.30.53") && !strcmp(status.gateway,"10.1.30.1"));
    assert(!strcmp(status.dns,"1.1.1.1") && !strcmp(status.mac,"02:00:00:12:34:56"));
    assert(!strcmp(status.hostname,"kubik-abcdef") && !strcmp(status.ssid,"saved-network"));
    status_dns=false; wifi_status(&status); assert(status.connected && !status.dns[0]); status_dns=true;
    status_up=false; wifi_status(&status); assert(!status.connected && !status.ip[0] && !status.gateway[0] && !status.dns[0]);
    status_up=true; associated=false; wifi_status(&status); assert(!status.connected && !status.ip[0]); associated=true;

    on_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL); assert(timer_us==1000000 && timer_armed);
    clock_ms=259999; assert(!wifi_take_recovery_request());
    // A repeated disconnect event must not postpone setup forever.
    disconnected(WIFI_REASON_NO_AP_FOUND);
    clock_ms=260000; assert(wifi_take_recovery_request());
    on_event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip); assert(!wifi_take_recovery_request() && !timer_armed);
    // Tick wrap must preserve the 60 second delay.
    clock_ms=UINT32_MAX-10000; on_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
    clock_ms=49998; assert(!wifi_take_recovery_request());
    clock_ms=49999; assert(wifi_take_recovery_request());
    for(int i=0;i<10;i++) on_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
    assert(timer_us==30000000);
    on_event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);

    // wifi_try switches networks; our own disconnect of the old one schedules no retry of it.
    int t0=timers, c0=connects;
    wifi_try("trial-net","trial-pass");
    assert(disconnects==1 && !wifi_sta_connected() && !strcmp(sta_ssid,"trial-net"));
    assert(timers==t0+1 && timer_us==300000 && timer_armed); // only the trial's own first attempt
    assert(wifi_attempts()==0 && wifi_last_reason()==0);
    fire_timer(); assert(connects==c0+1 && !strcmp(connect_ssid,"trial-net"));
    // Failed attempts are counted with their reason; each is retried on the trial network.
    disconnected(WIFI_REASON_AUTH_FAIL); assert(wifi_attempts()==1 && wifi_last_reason()==WIFI_REASON_AUTH_FAIL);
    fire_timer(); assert(!strcmp(connect_ssid,"trial-net"));
    disconnected(WIFI_REASON_NO_AP_FOUND); assert(wifi_attempts()==2 && wifi_last_reason()==WIFI_REASON_NO_AP_FOUND);
    // A deauth of our own (ASSOC_LEAVE) is not a failed attempt.
    disconnected(WIFI_REASON_ASSOC_LEAVE); assert(wifi_attempts()==2 && wifi_last_reason()==WIFI_REASON_ASSOC_LEAVE);
    on_event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip); assert(wifi_sta_connected());
    // Real driver: the old network's ASSOC_LEAVE arrives after wifi_try returned.
    sync_leave=false; wifi_try(NULL,NULL);
    assert(!strcmp(sta_ssid,"saved-network") && wifi_attempts()==0 && timer_us==300000);
    disconnected(WIFI_REASON_ASSOC_LEAVE); assert(wifi_attempts()==0);
    fire_timer(); assert(!strcmp(connect_ssid,"saved-network")); // any retry targets the new network
    sync_leave=true;
    wifi_try("candidate-pending", "candidate-password"); fire_timer();
    assert(!s_connected); // association is underway but no GOT_IP callback has run
    int before_cancel = disconnects;
    s_sta_armed = false; // first setup has not armed saved-profile recovery yet
    wifi_try(NULL, NULL);
    assert(disconnects == before_cancel + 1 && !s_trial && s_sta_armed);
    on_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip); // queued candidate event after cancellation
    assert(!wifi_sta_connected() && timer_armed);
    fire_timer(); assert(!strcmp(connect_ssid, "saved-network"));
    on_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip); assert(wifi_sta_connected());
    wifi_try("",""); fire_timer(); c0=connects; // empty trial: nothing to join, nothing retried
    disconnected(WIFI_REASON_NO_AP_FOUND); assert(!timer_armed); fire_timer(); assert(connects==c0);
    wifi_try(NULL,NULL); assert(!strcmp(sta_ssid,"saved-network") && timer_armed);

    // Three bounded failures advance to the next saved SSID directly, including a hidden one.
    strcpy(g_settings.wifi_profiles[1].ssid,"hidden-network");
    strcpy(g_settings.wifi_profiles[1].password,"hidden-password");
    g_settings.wifi_profile_count=2;
    wifi_try(NULL,NULL);
    for (int i=0;i<3;i++) disconnected(WIFI_REASON_NO_AP_FOUND);
    assert(!strcmp(sta_ssid,"hidden-network") && timer_armed && timer_us==300000);
    fire_timer(); assert(!strcmp(connect_ssid,"hidden-network"));
    for (int i=0;i<3;i++) disconnected(WIFI_REASON_NO_AP_FOUND);
    assert(!strcmp(sta_ssid,"saved-network") && timer_armed);

    // An intentional radio stop clears stale association state and blocks manager restarts until resumed.
    int starts_before_stop=starts, stops_before=stops;
    wifi_radio_stop(); assert(stops==stops_before+1 && !wifi_sta_connected() && !wifi_take_recovery_request());
    wifi_start_sta(); assert(starts==starts_before_stop);
    on_event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip); assert(!wifi_sta_connected());
    wifi_radio_resume(false); wifi_start_sta(); assert(starts==starts_before_stop+1);
    on_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_START,NULL); assert(!strcmp(connect_ssid,"saved-network"));

    // Setup access point: RFC 8910 portal URI, AP+STA, client counting.
    wifi_ap_start("Kubik-abcdef","setup-pass");
    assert(ap_netifs==1 && !strcmp(portal_uri,"http://192.168.4.1/") && mode==WIFI_MODE_APSTA);
    assert(!strcmp(ap_ssid,"Kubik-abcdef") && ap_cfg.ap.ssid_len==12 && ap_cfg.ap.authmode==WIFI_AUTH_WPA2_PSK);
    assert(wifi_ap_clients()==0);
    on_event(NULL,WIFI_EVENT,WIFI_EVENT_AP_STACONNECTED,NULL); on_event(NULL,WIFI_EVENT,WIFI_EVENT_AP_STACONNECTED,NULL);
    assert(wifi_ap_clients()==2);
    for(int i=0;i<3;i++) on_event(NULL,WIFI_EVENT,WIFI_EVENT_AP_STADISCONNECTED,NULL);
    assert(wifi_ap_clients()==0); // never negative
    on_event(NULL,WIFI_EVENT,WIFI_EVENT_AP_STACONNECTED,NULL);
    wifi_ap_start("Kubik-abcdef","setup-pass"); assert(ap_netifs==1 && wifi_ap_clients()==0);
    on_event(NULL,WIFI_EVENT,WIFI_EVENT_AP_STACONNECTED,NULL);
    wifi_ap_stop(); assert(mode==WIFI_MODE_STA && wifi_ap_clients()==0);
    wifi_ap_stop(); assert(mode==WIFI_MODE_STA);

    // Scan pauses retries, merges duplicates, skips hidden, respects cap, then resumes retries.
    wifi_net_t nets[3]; int d0=disconnects;
    assert(!wifi_sta_connected());
    int n=wifi_scan(nets,3);
    assert(scanning_seen && disconnects==d0+1 && n==3);
    assert(!strcmp(nets[0].ssid,"home") && nets[0].rssi==-40 && nets[0].secure);
    assert(!strcmp(nets[1].ssid,"cafe") && !nets[1].secure && !strcmp(nets[2].ssid,"office"));
    assert(timer_armed && timer_us==500000);
    scan_fails=true; assert(wifi_scan(nets,3)==0 && timer_armed); scan_fails=false;
    wifi_radio_stop();
    strcpy(g_settings.wifi_profiles[0].ssid,"absent");
    strcpy(g_settings.wifi_profiles[1].ssid,"office");
    strcpy(g_settings.wifi_profiles[1].password,"office-password");
    g_settings.wifi_profile_count=2;
    strcpy(g_settings.wifi_ssid,"absent");
    clock_ms=200000; wifi_radio_resume(true); wifi_start_sta();
    assert(wifi_polling() && !strcmp(sta_ssid,"office"));
    assert(!strcmp(g_settings.wifi_profiles[0].ssid,"absent"));
    for (int attempt=0; attempt<3; attempt++) { assert(wifi_link_attempt_allowed()); wifi_link_attempt_started(); }
    assert(!wifi_link_attempt_allowed());
    clock_ms=209999; assert(!wifi_poll_join_expired());
    clock_ms=210000; assert(!wifi_poll_join_expired()); // first AP handshake can time out before a retry succeeds
    disconnected(204); assert(timer_armed); // WIFI_REASON_HANDSHAKE_TIMEOUT
    clock_ms=211000; c0=connects; fire_timer(); assert(connects==c0+1);
    clock_ms=212000;
    on_event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip); assert(!wifi_poll_join_expired());
    wifi_radio_stop(); clock_ms=220000; wifi_radio_resume(true);
    assert(wifi_link_attempt_allowed()); // exhausted budget resets for the next check
    scan_fails=true; wifi_start_sta(); scan_fails=false;
    assert(!strcmp(sta_ssid,"office"));
    clock_ms=239999; assert(!wifi_poll_join_expired());
    clock_ms=240000; assert(wifi_poll_join_expired());
    wifi_radio_resume(false); assert(!wifi_polling() && wifi_link_attempt_allowed());
    for (int i=0; i<1000; i++) {
        stale_start_test(); in_flight_start_test(); scan_and_ip_race_test(); power_save_race_test();
    }
    puts("wifi: saved-profile round robin with hidden SSIDs, intentional radio stop/resume, 60s recovery, bounded trial retry, AP clients/portal and scan passed");
}
'''
with tempfile.TemporaryDirectory(prefix="kubik-wifi-") as tmp:
    src = Path(tmp) / "wifi.c"
    exe = Path(tmp) / "wifi"
    src.write_text(mock + source + test)
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-pthread",
                    "-I" + str(root / "firmware/main"), str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
