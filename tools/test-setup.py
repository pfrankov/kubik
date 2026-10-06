#!/usr/bin/env python3
"""Run the real setup state machine against small host-side platform mocks."""
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
SETUP = ROOT / "firmware/main/setup.c"

MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>
#include "setup.h"
#include "wifi.h"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef struct cJSON cJSON;
typedef struct { bool muse; char sdk_token[96]; } setup_agent_choice_t;
static int muse_store_saved_state(void) { return 0; }
static int muse_store_state(void) { return 0; }
#define MUSE_OFF 0
static esp_err_t setup_agent_save(const setup_agent_choice_t *choice) { (void)choice; return ESP_OK; }
static const setup_agent_choice_t host_choice;
static const char *setup_validate_credentials(const char *ssid,const char *pass);
static const char *setup_agent_choice(const cJSON *request, setup_agent_choice_t *choice);
static cJSON *setup_read_request(void *request);
static void muse_json_clear(cJSON *object);
static bool muse_sdk_token_valid(const char *token);
static int muse_store_begin(const char *token);
static int muse_store_select(bool muse);
static void muse_store_wipe(void *data,size_t size);
static cJSON *muse_json_parse(const char *data,size_t size);
static const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object,const char *key);
static char *cJSON_PrintUnformatted(const cJSON *j);
static void cJSON_Delete(cJSON *j);
static void cJSON_free(void *p);
static cJSON *cJSON_CreateObject(void);
static cJSON *cJSON_CreateTrue(void);
static cJSON *cJSON_AddArrayToObject(cJSON *j, const char *name);
static cJSON *cJSON_CreateString(const char *value);
static cJSON *cJSON_AddStringToObject(cJSON *j, const char *name, const char *value);
static cJSON *cJSON_AddBoolToObject(cJSON *j, const char *name, bool value);
static cJSON *cJSON_AddNumberToObject(cJSON *j, const char *name, double value);
static void cJSON_AddItemToArray(cJSON *array, cJSON *item);
static cJSON *cJSON_Parse(const char *text);
static cJSON *cJSON_GetObjectItem(const cJSON *j, const char *name);
static const char *cJSON_GetStringValue(const cJSON *j);

typedef struct { int unused; } mock_mutex_t;
typedef mock_mutex_t *SemaphoreHandle_t;
typedef void *httpd_handle_t;
typedef void *TaskHandle_t;
static mock_mutex_t mutex;
static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool contention_test, owner_locked, contender_waiting;
static __thread int lock_role;
enum { ROLE_NONE, ROLE_OWNER, ROLE_CONTENDER };
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex; }
static int xSemaphoreTake(SemaphoreHandle_t m, unsigned timeout) {
    (void)timeout; assert(m == &mutex);
    if (contention_test && lock_role == ROLE_CONTENDER) {
        pthread_mutex_lock(&gate_mutex);
        contender_waiting = true;
        pthread_cond_broadcast(&gate_cond);
        pthread_mutex_unlock(&gate_mutex);
    }
    pthread_mutex_lock(&test_mutex);
    if (contention_test && lock_role == ROLE_OWNER) {
        pthread_mutex_lock(&gate_mutex);
        owner_locked = true;
        pthread_cond_broadcast(&gate_cond);
        while (!contender_waiting) pthread_cond_wait(&gate_cond, &gate_mutex);
        pthread_mutex_unlock(&gate_mutex);
    }
    return 1;
}
static int xSemaphoreGive(SemaphoreHandle_t m) {
    assert(m == &mutex); pthread_mutex_unlock(&test_mutex); return 1;
}
#define portMAX_DELAY 0xffffffffu
static void vTaskDelete(TaskHandle_t task) { (void)task; }
static int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *out) {
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)out;
    return 0;
}

typedef struct { int content_len; } httpd_req_t;
struct cJSON { int unused; };
typedef struct {
    bool (*uri_match_fn)(const char *, const char *);
    int stack_size, max_open_sockets, lru_purge_enable, max_uri_handlers, task_priority;
} httpd_config_t;
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
typedef int httpd_err_code_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_404_NOT_FOUND 404
static int httpd_start(httpd_handle_t *h, const httpd_config_t *c) { (void)h; (void)c; return ESP_FAIL; }
static int httpd_stop(httpd_handle_t h) { (void)h; return ESP_OK; }
static int httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u) { (void)h; (void)u; return 0; }
static int httpd_register_err_handler(httpd_handle_t h, int code,
                                      esp_err_t (*fn)(httpd_req_t *, httpd_err_code_t)) {
    (void)h; (void)code; (void)fn; return 0;
}
static bool (*httpd_uri_match_wildcard)(const char *, const char *);
static bool request_on_ap;
static int sent_status, sent_headers;
static const char *sent_body, *sent_type, *sent_encoding, *sent_cache;
static ssize_t sent_length;
static int test_getsockname(int fd, struct sockaddr *address, socklen_t *length) {
    (void)fd; (void)length;
    struct sockaddr_in *ip = (struct sockaddr_in *)address;
    memset(ip, 0, sizeof *ip); ip->sin_family = AF_INET;
    const uint8_t bytes[4] = {192, 168, request_on_ap ? 4 : 1, 1};
    memcpy(&ip->sin_addr.s_addr, bytes, sizeof bytes);
    return 0;
}
#define getsockname test_getsockname
static int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return -1; }
static esp_err_t httpd_resp_send_err(httpd_req_t *r, int code, const char *msg) { (void)r; (void)msg; sent_status = code; return ESP_FAIL; }
static esp_err_t httpd_resp_send_500(httpd_req_t *r) { (void)r; return ESP_FAIL; }
static esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *s) { (void)r; sent_type = s; return ESP_OK; }
static esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v) {
    (void)r; sent_headers++;
    if (!strcmp(k, "Content-Encoding")) sent_encoding = v;
    if (!strcmp(k, "Cache-Control")) sent_cache = v;
    return ESP_OK;
}
static esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_OK; }
static esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_OK; }
static esp_err_t httpd_resp_send(httpd_req_t *r, const char *s, ssize_t n) { (void)r; sent_body = s; sent_length = n; return ESP_OK; }
static int httpd_req_recv(httpd_req_t *r, char *p, size_t n) { (void)r; (void)p; (void)n; return -1; }

typedef struct { char ssid[33], password[65]; } wifi_profile_t;
typedef struct {
    wifi_profile_t wifi_profiles[8];
    uint8_t wifi_profile_count;
    char wifi_ssid[33], wifi_pass[65], server_url[128], name[32];
} settings_t;
settings_t g_settings = {.server_url = "ws://server/kubik/v1"};
char g_device_id[20] = "kubik-abcdef";
static bool settings_server_valid(const char *url) { return url && (!url[0] || !strncmp(url, "ws://", 5)); }
static int settings_wifi_find(const char *ssid) {
    for (int i = 0; i < g_settings.wifi_profile_count; i++)
        if (!strcmp(g_settings.wifi_profiles[i].ssid, ssid)) return i;
    return -1;
}
static esp_err_t save_result = ESP_OK;
static esp_err_t settings_save_connection(const char *ssid, const char *pass, const char *url) {
    if (save_result == ESP_OK) {
        snprintf(g_settings.wifi_ssid, sizeof g_settings.wifi_ssid, "%s", ssid);
        snprintf(g_settings.wifi_pass, sizeof g_settings.wifi_pass, "%s", pass);
        snprintf(g_settings.server_url, sizeof g_settings.server_url, "%s", url);
    }
    return save_result;
}

#define QR_MAX_N 1
static int qr_make(const char *s, uint8_t *out) { (void)s; (void)out; return 0; }
static uint32_t esp_random(void) { return 0; }
static int64_t test_time_us;
static int64_t esp_timer_get_time(void) { return test_time_us; }

static bool station_connected;
static int ap_clients;
static int try_calls;
static char tried_ssid[33], tried_pass[65];
void wifi_try(const char *ssid, const char *pass) {
    try_calls++;
    snprintf(tried_ssid, sizeof tried_ssid, "%s", ssid ? ssid : "");
    snprintf(tried_pass, sizeof tried_pass, "%s", pass ? pass : "");
}
bool wifi_sta_connected(void) { return station_connected; }
int wifi_ap_clients(void) { return ap_clients; }
int wifi_scan(wifi_net_t *out, int cap) { (void)out; (void)cap; return 0; }
int wifi_attempts(void) { return 0; }
uint8_t wifi_last_reason(void) { return 0; }
void wifi_ap_stop(void) { }
void wifi_ap_start(const char *ssid, const char *pass) { (void)ssid; (void)pass; }

static const char *queue_connection(const char *ssid, const char *password, const char *url, const setup_agent_choice_t *agent);
typedef struct { bool claim; int role; const char *error; bool claimed; } action_t;
static void *run_action(void *arg) {
    action_t *a = arg;
    lock_role = a->role;
    if (a->claim) a->claimed = setup_claim_idle_close();
    else a->error = queue_connection(a->role == ROLE_OWNER ? "home-net" : "late-net",
                                     a->role == ROLE_OWNER ? "correct-pass" : "another-pass", NULL, &host_choice);
    return NULL;
}
static void race_pair(bool owner_claims) {
    action_t owner = {.claim = owner_claims, .role = ROLE_OWNER};
    action_t contender = {.claim = !owner_claims, .role = ROLE_CONTENDER};
    pthread_t first, second;
    contention_test = true;
    owner_locked = contender_waiting = false;
    assert(pthread_create(&first, NULL, run_action, &owner) == 0);
    pthread_mutex_lock(&gate_mutex);
    while (!owner_locked) pthread_cond_wait(&gate_cond, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
    assert(pthread_create(&second, NULL, run_action, &contender) == 0);
    assert(pthread_join(first, NULL) == 0);
    assert(pthread_join(second, NULL) == 0);
    contention_test = false;
    if (owner_claims) {
        assert(owner.claimed && contender.error && !strcmp(contender.error, "busy"));
    } else {
        assert(!owner.error && !contender.claimed);
    }
}
'''

CASE = r'''
int main(void) {
    httpd_req_t request = {0};
    const char body[] = {0x1f, (char)0x8b, 0, 7};
    const char *mime = "text/html; charset=utf-8";
    assert(asset_get(&request, mime, body, body + sizeof body, true) == ESP_FAIL);
    assert(sent_status == 403 && !sent_headers && !sent_type && !sent_body);
    request_on_ap = true;
    assert(asset_get(&request, mime, body, body + sizeof body, true) == ESP_OK);
    assert(sent_body == body && sent_length == sizeof body && sent_type == mime);
    assert(!strcmp(sent_encoding, "gzip") && !strcmp(sent_cache, "no-store"));
    sent_cache = NULL; sent_headers = 0;
    assert(asset_get(&request, "text/css", body, body + sizeof body, false) == ESP_OK);
    assert(sent_headers == 1 && !sent_cache && !strcmp(sent_type, "text/css"));
    s_mtx = &mutex;
    s_running = true;
    s_phase = SETUP_OPEN;
    s_scan_wanted = false;
    strcpy(g_settings.wifi_profiles[0].ssid, "hidden-home");
    strcpy(g_settings.wifi_profiles[0].password, "saved-password");
    g_settings.wifi_profile_count = 1;

    // Hold the queue lock while recovery attempts its claim. The request is
    // accepted first and must reach wifi_try intact.
    race_pair(false);
    assert(s_req.pending);
    assert(setup_poll() == SETUP_TRYING);
    assert(try_calls == 1);
    assert(!strcmp(tried_ssid, "home-net"));
    assert(!strcmp(tried_pass, "correct-pass"));

    // Hold the close claim lock while the phone submits: it must be rejected.
    s_phase = SETUP_OPEN;
    s_closing = false;
    race_pair(true);
    assert(!s_req.pending);
    assert(try_calls == 1);

    // A missing url keeps the saved server; an empty one asks for LAN discovery.
    s_phase = SETUP_OPEN;
    s_closing = false;
    assert(!queue_connection("home-net", "correct-pass", NULL, &host_choice) && !strcmp(s_req.url, g_settings.server_url));
    s_req.pending = false;
    assert(!queue_connection("hidden-home", "", NULL, &host_choice) && !strcmp(s_req.pass, "saved-password"));
    s_req.pending = false;
    assert(!queue_connection("home-net", "correct-pass", "", &host_choice) && s_req.pending && !s_req.url[0]);
    s_req.pending = false;
    assert(!strcmp(queue_connection("home-net", "correct-pass", "https://x", &host_choice), "server_format"));
    // A joined network is not committed until NVS succeeds. Failure must exit
    // trial mode and restore the previous profile; success must keep the new link.
    s_phase = SETUP_TRYING; station_connected = true; save_result = ESP_FAIL;
    strcpy(s_try.pass, "candidate-password");
    int before_failure = try_calls;
    assert(setup_poll() == SETUP_FAILED);
    assert(try_calls == before_failure + 1 && !tried_ssid[0] && !tried_pass[0]);
    assert(!s_try.pass[0]);
    assert(!strcmp(g_settings.wifi_profiles[0].password, "saved-password"));
    s_phase = SETUP_TRYING; save_result = ESP_OK;
    strcpy(s_try.ssid, "committed-net");
    assert(setup_poll() == SETUP_DONE && try_calls == before_failure + 1);

    int before_close = try_calls;
    setup_stop(); // Commit won: DONE retains the newly saved profile.
    assert(try_calls == before_close + 1 && s_phase == SETUP_OFF);
    assert(!strcmp(g_settings.wifi_ssid, "committed-net"));
    assert(!tried_ssid[0]); // NULL leaves trial mode and selects the now committed profile.
    assert(!strcmp(queue_connection("late-net", "late-password", "", &host_choice), "busy"));
    s_running = true; s_closing = false; s_phase = SETUP_TRYING;
    strcpy(s_try.pass, "uncommitted-password");
    setup_stop(); // Back won: TRYING restores the committed profile and wipes the candidate.
    assert(try_calls == before_close + 2 && !tried_ssid[0] && !s_try.pass[0]);
    assert(!strcmp(queue_connection("late-net", "late-password", "", &host_choice), "busy"));

    puts("setup: saved SSID list without secrets, hidden saved-network empty-password retention, "
         "pending request/close race, empty LAN server and invalid address passed");
}
'''


def main():
    source_text = SETUP.read_text()
    assert 'cJSON_AddArrayToObject(j, "saved_nets")' in source_text
    assert 'cJSON_CreateString(g_settings.wifi_profiles[i].ssid)' in source_text
    assert 'cJSON_AddStringToObject(j, "saved",' not in source_text
    request_source = (ROOT / "firmware/main/setup_request.c").read_text()
    source_text += "\n" + request_source[request_source.index("const char *setup_validate_credentials("):request_source.index("const char *setup_agent_choice(")]
    production = "\n".join(
        line for line in source_text.splitlines()
        if not line.startswith("#include")
    )
    with tempfile.TemporaryDirectory(prefix="kubik-setup-") as tmp:
        cfile = Path(tmp) / "setup-test.c"
        exe = Path(tmp) / "setup-test"
        cfile.write_text(MOCKS + production + CASE)
        linker_gc = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
        subprocess.run([
            "cc", "-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
            "-pthread", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-undefined-internal",
            str(cfile), linker_gc, "-I" + str(ROOT / "firmware/main"), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
