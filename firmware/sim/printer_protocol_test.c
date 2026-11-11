#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "printer_protocol.h"
#include "printer_work.h"
#include "printer_platform.h"
#include "printer_job.h"
#include "printer_guard.h"
#include "printer_status.h"
#include "peripage_ble.h"
#include "ble_runtime.h"
#include "link_hello.h"
#include "settings.h"

#define ESP_LOGW(...) ((void)0)
static pthread_mutex_t route_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t session = 7;
static unsigned attempts, dispatched, opens, closes, packets, rows, launches, reaps, releases, cancels;
static unsigned fail_packet, change_session_packet;
static bool rx_active, send_ok = true, radio_owned, close_ok = true, close_stuck, launch_ok = true;
static bool work_finished, booted, fail_alloc, lease, defer_work;
static unsigned packet_ms, finishes, budget_calls, planned_packets;
static bool finish_ok = true;
static uint8_t battery_pct = 100;
static uint8_t finish_battery_pct = 100;
static printer_status_t status_at_close;
static int64_t clock_ms;
static char sent[64][2048];
static void (*callback)(void *);
static void *callback_context;
static printer_app_state_t app_state;
static peripage_result_t open_result = PERIPAGE_OK;
static jmp_buf restarted;
static const char RID[] = "0123456789abcdef0123456789abcdef";
static bool printed_ink;
static size_t last_job_allocation;

void *printer_test_calloc(size_t count, size_t size) {
    if (fail_alloc) { fail_alloc = false; return NULL; }
    if (size && count > SIZE_MAX / size) return NULL;
    last_job_allocation=count*size;
    void *p = malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
uint32_t link_session(void) { return session; }
bool link_send_json_in_session(const char *json, uint32_t expected) {
    assert(!rx_active && expected == session);
    if (strstr(json,"\"status\":\"ready\"") || strstr(json,"\"status\":\"sent\"")) assert(!lease);
    assert(!pthread_mutex_trylock(&route_lock));
    assert(attempts < 64 && strlen(json) < sizeof sent[0]);
    snprintf(sent[attempts++], sizeof sent[0], "%s", json);
    pthread_mutex_unlock(&route_lock); return send_ok;
}
static void dispatch_json(cJSON *request, const char *type) {
    (void)request; assert(strcmp(type, "printer")); dispatched++;
}
static bool agent_protocol_has_nul_escape(const char *json, size_t len) {
    (void)len; return strstr(json, "\\u0000") != NULL;
}
/* PRODUCTION_ON_JSON */
static int64_t now_ms(void) { return clock_ms; }
static bool tick_power_off(int64_t now) { (void)now; return true; }
static void tick_ui_timeouts(int64_t now) { (void)now; }
static void app_guide_tick(void) {}
static void tick_network_power(int64_t now) { (void)now; }
static bool tick_recovery(void) { return false; }
static void status_tick(void) {}
static void tick_pickup_sound(int64_t now) { (void)now; }
static void tick_connection(int64_t now) { (void)now; }
static void tick_playback(int64_t now) { (void)now; }
static void tick_conversation(int64_t now) { (void)now; }
static void tick_sleep(int64_t now) { (void)now; }
/* PRODUCTION_APP_TICK */

void printer_app_state(printer_app_state_t *state) { *state = app_state; state->session = session; }
int64_t printer_platform_now_ms(void) { return clock_ms; }
int printer_platform_reset_reason(void) { return 1; }
void printer_platform_boot(void) { if (!booted) { printer_guard_boot(true); booted = true; } }
bool printer_platform_launch(void (*run)(void *), void *context) {
    if (!launch_ok) return false;
    assert(!callback && !lease); launches++; callback = run; callback_context = context;
    work_finished = false; lease = true; return true;
}
bool printer_platform_finished(void) { return work_finished; }
void printer_platform_reap(void) {
    assert(work_finished && callback && !radio_owned); callback = NULL; callback_context = NULL;
    work_finished = false; reaps++;
}
void printer_platform_release(void) { assert(!callback && lease); lease = false; releases++; }
void printer_platform_radio_fault(void) { longjmp(restarted, 1); }
static void finish_work(void) {
    assert(callback); callback(callback_context); work_finished = true;
}
void printer_platform_wait_tick(void) {
    clock_ms++;
    if (callback && !work_finished && !defer_work) finish_work();
}
peripage_result_t peripage_open(peripage_info_t *info, uint32_t timeout) {
    assert(!rx_active && lease && timeout >= 4000 && timeout <= 12000); opens++; radio_owned = true;
    *info = (peripage_info_t){.name="PeriPage+test", .firmware="V1.12_304dpi", .hardware="v3.38.21_AY",
        .dpi=304, .feed_dpi=152, .width_dots=576, .mtu=136, .battery_pct=battery_pct, .charging=-1};
    return open_result;
}
bool peripage_write_packet(const uint8_t *data, size_t size, uint32_t timeout) {
    assert(radio_owned && lease && !rx_active && timeout > 0 && timeout <= 3000);
    packets++; clock_ms += packet_ms;
    if (change_session_packet == packets) session++;
    if (size == 72) {
        rows++;
        for (unsigned i = 0; i < 72; i++) printed_ink |= data[i] != 0;
    } else if (size == 16) {
        static const uint8_t reset[16] = {0x10,0xff,0xfe,1}; assert(!memcmp(data,reset,16));
    } else if (size == 5) {
        static const uint8_t density[5] = {0x10,0xff,0x10,0,2}; assert(!memcmp(data,density,5));
    } else {
        assert(size == 8 && !memcmp(data,"\x1d\x76\x30\x00\x48\x00",6));
        assert(data[6] && !data[7]);
    }
    return packets != fail_packet;
}
uint32_t peripage_packet_stream_budget_ms(size_t count) {
    budget_calls++; planned_packets = count;
    return count * 20 + 6000;
}
bool peripage_finish_packet_stream(uint32_t timeout, peripage_info_t *info) {
    assert(radio_owned && lease && !rx_active && packets && timeout > 0 && timeout <= 3000);
    finishes++;if(finish_ok){info->battery_pct=finish_battery_pct;info->charging=-1;}return finish_ok;
}
bool peripage_close(void) {
    printer_status_snapshot(&status_at_close);closes++;if (!close_stuck) radio_owned = false;return close_ok;
}
void peripage_cancel(void) { cancels++; }
bool ble_runtime_owned(ble_runtime_owner_t owner) {
    return owner == BLE_RUNTIME_PRINTER ? radio_owned : owner == BLE_RUNTIME_NONE && !radio_owned;
}

static void fresh(void) {
    assert(!printer_work_busy() && !lease && !callback);
    attempts = dispatched = opens = closes = packets = rows = launches = reaps = releases = cancels = 0;
    fail_packet = change_session_packet = packet_ms = finishes = budget_calls = planned_packets = 0;
    finish_ok = true;
    battery_pct=finish_battery_pct=100;printer_status_set_visible(false);
    send_ok = close_ok = launch_ok = true; close_stuck = fail_alloc = printed_ink = false;
    open_result = PERIPAGE_OK; session++; clock_ms += 301000;
    app_state = (printer_app_state_t){.online=true,.enabled=true,.can_start=true};
    printer_guard_reset();
    app_tick(); // Observe this session before tests alter automatic policy.
}
static void receive_bytes(const char *raw, size_t length) {
    unsigned before = attempts;
    assert(!pthread_mutex_lock(&route_lock)); rx_active = true; on_json(raw, length);
    rx_active = false; pthread_mutex_unlock(&route_lock); assert(attempts == before);
}
static void receive(const char *raw) { receive_bytes(raw, strlen(raw)); }
static void request(const char *rid, const char *fields) {
    char raw[1800]; snprintf(raw,sizeof raw,"{\"t\":\"printer\",\"rid\":\"%s\",%s}",rid,fields); receive(raw);
}
static cJSON *reply(unsigned index, const char *status, const char *code, const char *outcome) {
    assert(index < attempts); cJSON *j = cJSON_Parse(sent[index]); assert(j);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j,"status")),status));
    if (code) assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j,"code")),code));
    else assert(!cJSON_GetObjectItem(j,"code"));
    if (outcome) assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j,"outcome")),outcome));
    else assert(!cJSON_GetObjectItem(j,"outcome"));
    assert(!cJSON_GetObjectItem(j,"text")); return j;
}
static void finish_request(void) { app_tick(); assert(callback); finish_work(); app_tick(); }
static void invalid(const char *fields, bool printing) {
    unsigned before = attempts; request(RID,fields); app_tick(); assert(attempts == before+1);
    cJSON_Delete(reply(before,"error","invalid_request",printing?"not_sent":NULL));
    assert(!opens && !printer_work_busy());
}

#include "printer_parse_cases.h"
#include "printer_work_cases.h"
#include "printer_status_cases.h"
#include "printer_emoji_protocol_cases.h"

settings_t g_settings;
char g_device_id[20] = "kubik-782900";
static bool key_ready = true;
bool devkey_init(void) { return key_ready; }
const char *devkey_public(void) { return key_ready ? "synthetic-public-key" : ""; }
bool devkey_sign_challenge(const char *nonce, const char *bind, char *out, size_t cap) {
    (void)nonce; (void)bind; snprintf(out, cap, "synthetic-signature"); return true;
}
static void hello_descriptor(void) {
    snprintf(g_settings.name,sizeof g_settings.name,"My Kubik");
    snprintf(g_settings.server_url,sizeof g_settings.server_url,"kubik://example.test:18794");
    g_settings.volume=42;g_settings.ui_volume=57;g_settings.brightness=150;g_settings.guide_done=true;
    settings_t before=g_settings;char hello[512];assert(link_make_hello(hello,sizeof hello));
    cJSON *j=cJSON_Parse(hello);assert(j);assert(cJSON_GetObjectItem(j,"printer")->valueint==2);
    assert(cJSON_GetObjectItem(j,"v")->valueint==5);assert(!memcmp(&before,&g_settings,sizeof before));cJSON_Delete(j);
    key_ready=false;assert(!link_make_hello(hello,sizeof hello));key_ready=true;assert(!link_make_hello(hello,16));
}
int main(void) {
    status_cache_unchecked();parser_fields();parser_text();parser_raw();
    status_cache_verified_and_failed();status_visible_polling();status_explicit_refresh_bound();status_print_final_reading();
    status_refresh_after_fault();
    emoji_required_and_foreign();emoji_bad_shape_and_sequences();emoji_bad_base64();emoji_owned_until_print();
    emoji_owned_eight();
    status_and_print(); job_errors();
    no_replay_and_sessions(); automatic_discovery(); discovery_voice_admission(); foreground_voice_unchanged();
    diagnostic_snapshot(); resource_and_deadline(); stream_budget_and_finish(); crash_guard(); hello_descriptor();
    puts("Printer v2: production RX/tick/parser/job/renderer/worker passed with no physical side-effect mocks in firmware");
}
