#include "setup.h"
#include "setup_request.h"
#include "muse_store.h"
#include "muse_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "qr.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "setup";

#define AP_IP_BYTES {192, 168, 4, 1}
#define PAGE_URL "http://192.168.4.1/"
#define TRY_TIMEOUT_MS 25000
#define TRY_MAX_ATTEMPTS 3
#define FAILED_SHOW_MS 5000
#define NETS_MAX 16

extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[] asm("_binary_portal_html_end");
extern const char portal_tess_start[] asm("_binary_portal_tess_js_start");
extern const char portal_tess_end[] asm("_binary_portal_tess_js_end");
extern const char portal_css_start[] asm("_binary_portal_css_start");
extern const char portal_css_end[] asm("_binary_portal_css_end");

static SemaphoreHandle_t s_mtx;
static httpd_handle_t s_http;
static TaskHandle_t s_dns_task;
static volatile bool s_running;
static bool s_closing;
static setup_reason_t s_why;
static setup_phase_t s_phase;
static setup_error_t s_error;
static int64_t s_phase_ms;
static char s_ap_ssid[16], s_ap_pass[9];
// The QR codes and the network list: 4.4 KB that only an open setup needs, taken by setup_start and given back by
// setup_stop (static they sat in the heap's way for the whole life of the device).
typedef struct {
    uint8_t qr[2][QR_MAX_N * QR_MAX_N];
    wifi_net_t nets[NETS_MAX], scan[NETS_MAX];
} setup_mem_t;
static setup_mem_t *s_mem;
static int s_qr_n[2];
static int s_nnets;
static bool s_scan_wanted;

// Candidate submitted by the page, applied by setup_poll on the app task.
static struct {
    bool pending;
    char ssid[33], pass[65], url[128];
    setup_agent_choice_t agent;
} s_req, s_try;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void lock(void) { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mtx); }
static void wipe(void *p, size_t n) {
    volatile uint8_t *b = p;
    while (n--) *b++ = 0;
}

static void set_phase(setup_phase_t p) {
    if (s_phase != p) ESP_LOGI(TAG, "phase %d -> %d", s_phase, p);
    s_phase = p;
    s_phase_ms = now_ms();
}

// ------------------------------------------------------------------ DNS: every name is Kubik
static void dns_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53)};
    const uint8_t ip[4] = AP_IP_BYTES;
    memcpy(&addr.sin_addr.s_addr, ip, 4);  // only the setup network, never the home LAN
    struct timeval tv = {.tv_sec = 0, .tv_usec = 400000};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
        ESP_LOGE(TAG, "dns socket failed");
    } else {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        uint8_t buf[300];
        while (s_running) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(sock, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
            if (n < 12 || (buf[2] & 0x80)) continue;  // not a query
            // Walk the single question to find its type.
            int p = 12;
            while (p < n && buf[p]) p += buf[p] + 1;
            if (p + 5 > n) continue;
            int qtype = (buf[p + 1] << 8) | buf[p + 2];
            int end = p + 5;
            buf[2] = 0x81; buf[3] = 0x80;  // response, recursion available, no error
            buf[6] = 0; buf[7] = 0; buf[8] = buf[9] = buf[10] = buf[11] = 0;
            if (qtype == 1) {  // A: this device
                buf[7] = 1;
                const uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, ip[0], ip[1], ip[2], ip[3]};
                memcpy(buf + end, ans, sizeof ans);
                end += sizeof ans;
            }
            sendto(sock, buf, end, 0, (struct sockaddr *)&from, fl);
        }
    }
    if (sock >= 0) close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

// ------------------------------------------------------------------ HTTP
// The page only exists on Kubik's own network: requests that reach the server
// over the home LAN (AP+STA) are refused.
static bool from_setup_net(httpd_req_t *r) {
    struct sockaddr_storage a;
    socklen_t l = sizeof a;
    if (getsockname(httpd_req_to_sockfd(r), (struct sockaddr *)&a, &l) != 0) return false;
    const uint8_t ip[4] = AP_IP_BYTES;
    if (a.ss_family == AF_INET) return !memcmp(&((struct sockaddr_in *)&a)->sin_addr.s_addr, ip, 4);
#if LWIP_IPV6
    if (a.ss_family == AF_INET6) {
        const uint8_t *b = ((struct sockaddr_in6 *)&a)->sin6_addr.s6_addr;
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        return !memcmp(b, mapped, 12) && !memcmp(b + 12, ip, 4);
    }
#endif
    return false;
}

static esp_err_t send_json(httpd_req_t *r, cJSON *j) {
    char *text = j ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (!text) return httpd_resp_send_500(r);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(r, text);
    cJSON_free(text);
    return err;
}

static esp_err_t redirect(httpd_req_t *r, httpd_err_code_t code) {
    // Captive-portal probes (Apple, Android, Windows) land here: send them to the page.
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", PAGE_URL);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, NULL, 0);
}

static esp_err_t asset_get(httpd_req_t *r, const char *type, const char *start, const char *end, bool no_store) {
    if (!from_setup_net(r)) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, NULL);
    httpd_resp_set_type(r, type);
    if (no_store) httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    return httpd_resp_send(r, start, end - start);
}

static esp_err_t page_get(httpd_req_t *r) {
    return asset_get(r, "text/html; charset=utf-8", portal_html_start, portal_html_end, true);
}

static esp_err_t tess_get(httpd_req_t *r) {
    return asset_get(r, "text/javascript; charset=utf-8", portal_tess_start, portal_tess_end, false);
}

static esp_err_t style_get(httpd_req_t *r) {
    return asset_get(r, "text/css; charset=utf-8", portal_css_start, portal_css_end, false);
}

static const char *phase_name(setup_phase_t p) {
    static const char *const n[] = {"off", "join", "open", "trying", "done", "failed"};
    return n[p];
}
static const char *error_name(setup_error_t e) {
    static const char *const n[] = {"", "password", "not_found", "other"};
    return n[e];
}

static esp_err_t state_get(httpd_req_t *r) {
    if (!from_setup_net(r)) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, NULL);
    cJSON *j = cJSON_CreateObject();
    lock();
    cJSON_AddStringToObject(j, "device", g_device_id);
    cJSON_AddStringToObject(j, "name", g_settings.name);
    cJSON_AddStringToObject(j, "server", g_settings.server_url);
    cJSON_AddStringToObject(j, "phase", phase_name(s_phase));
    cJSON_AddStringToObject(j, "error", error_name(s_error));
    cJSON_AddStringToObject(j, "trying", s_try.ssid);
    cJSON_AddBoolToObject(j, "scanning", s_scan_wanted);
    cJSON_AddBoolToObject(j, "muse_saved", muse_store_saved_state() != MUSE_OFF);
    cJSON_AddStringToObject(j, "agent", muse_store_state() != MUSE_OFF ? "Muse" : "OpenClaw");
    cJSON *saved_nets = cJSON_AddArrayToObject(j, "saved_nets");
    for (int i = 0; i < g_settings.wifi_profile_count; i++)
        cJSON_AddItemToArray(saved_nets, cJSON_CreateString(g_settings.wifi_profiles[i].ssid));
    cJSON *nets = cJSON_AddArrayToObject(j, "nets");
    for (int i = 0; s_mem && i < s_nnets; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "s", s_mem->nets[i].ssid);
        cJSON_AddNumberToObject(o, "r", s_mem->nets[i].rssi);
        cJSON_AddBoolToObject(o, "l", s_mem->nets[i].secure);
        cJSON_AddItemToArray(nets, o);
    }
    unlock();
    return send_json(r, j);
}

static esp_err_t scan_post(httpd_req_t *r) {
    if (!from_setup_net(r)) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, NULL);
    lock();
    s_scan_wanted = true;
    unlock();
    return send_json(r, cJSON_CreateTrue());
}

static esp_err_t fail(httpd_req_t *r, const char *code) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "error", code);
    return send_json(r, j);
}

static const char *queue_connection(const char *ssid, const char *password, const char *url, const setup_agent_choice_t *agent) {
    lock();
    // An empty password keeps the saved one for the same network.
    int saved_index = settings_wifi_find(ssid);
    bool keep_password = (!password || !password[0]) && saved_index >= 0;
    // No "url" keeps the saved server; "" finds OpenClaw on the home network.
    const char *server_url = url ? url : g_settings.server_url;
    const char *error = NULL;
    if (!settings_server_valid(server_url)) error = "server_format";
    else if (s_closing || s_phase == SETUP_TRYING || s_phase == SETUP_DONE) error = "busy";
    else {
        memset(&s_req, 0, sizeof s_req);
        snprintf(s_req.ssid, sizeof s_req.ssid, "%s", ssid);
        snprintf(s_req.pass, sizeof s_req.pass, "%s", keep_password ?
                 g_settings.wifi_profiles[saved_index].password : (password ? password : ""));
        snprintf(s_req.url, sizeof s_req.url, "%s", server_url);
        s_req.agent = *agent;
        s_req.pending = true;
        s_error = SETUP_ERR_NONE;
    }
    unlock();
    return error;
}

static const char *queue_connect(cJSON *request) {
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(request, "ssid"));
    const char *password = cJSON_GetStringValue(cJSON_GetObjectItem(request, "pass"));
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(request, "url"));
    setup_agent_choice_t agent;
    const char *error = setup_agent_choice(request, &agent);
    if (!error) error = setup_validate_credentials(ssid, password);
    if (!error) error = queue_connection(ssid, password, agent.muse ? g_settings.server_url : url, &agent);
    wipe(&agent, sizeof agent); return error;
}

static esp_err_t connect_post(httpd_req_t *r) {
    if (!from_setup_net(r)) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, NULL);
    cJSON *request = setup_read_request(r);
    if (!request) return fail(r, "bad_request");
    const char *error = queue_connect(request);
    muse_json_clear(request);
    if (error) return fail(r, error);
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return send_json(r, ok);
}

static void http_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;
    cfg.task_priority = 4;
    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "http server failed");
        s_http = NULL;
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/tess.js", .method = HTTP_GET, .handler = tess_get},
        {.uri = "/setup.css", .method = HTTP_GET, .handler = style_get},
        {.uri = "/api/state", .method = HTTP_GET, .handler = state_get},
        {.uri = "/api/scan", .method = HTTP_POST, .handler = scan_post},
        {.uri = "/api/connect", .method = HTTP_POST, .handler = connect_post},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(s_http, &uris[i]);
    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, redirect);
}

// ------------------------------------------------------------------ state machine
static void make_qrs(void) {
    char text[64];
    snprintf(text, sizeof text, "WIFI:T:WPA;S:%s;P:%s;;", s_ap_ssid, s_ap_pass);
    s_qr_n[0] = qr_make(text, s_mem->qr[0]);
    wipe(text, sizeof text);
    s_qr_n[1] = qr_make(PAGE_URL, s_mem->qr[1]);
}

static void scan_now(void) {
    if (!s_mem) return;
    int n = wifi_scan(s_mem->scan, NETS_MAX);
    lock();
    memcpy(s_mem->nets, s_mem->scan, sizeof s_mem->nets);
    s_nnets = n;
    s_scan_wanted = false;
    unlock();
    ESP_LOGI(TAG, "scan: %d networks", n);
}

void setup_start(setup_reason_t why) {
    if (s_running) return;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_mem) s_mem = calloc(1, sizeof *s_mem);
    if (!s_mem) { ESP_LOGE(TAG, "no memory for the setup"); return; }
    s_why = why;
    s_closing = false;
    s_error = SETUP_ERR_NONE;
    memset(&s_req, 0, sizeof s_req);
    memset(&s_try, 0, sizeof s_try);
    // Uppercase AP name and a fresh 8-digit password (the WPA2 minimum) for every session.
    const char *id = g_device_id + strlen(g_device_id) - 4;
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "KUBIK-%s", id);
    for (char *c = s_ap_ssid + 6; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c -= 32;
    for (size_t i = 0; i < sizeof s_ap_pass - 1; i++)
        s_ap_pass[i] = '0' + esp_random() % 10;
    s_ap_pass[sizeof s_ap_pass - 1] = 0;
    make_qrs();
    s_running = true;
    s_scan_wanted = true;
    set_phase(SETUP_JOIN);
    wifi_ap_start(s_ap_ssid, s_ap_pass);
    http_start();
    xTaskCreate(dns_task, "dns", 3072, NULL, 3, &s_dns_task);
    ESP_LOGI(TAG, "started (reason %d), network %s", why, s_ap_ssid);
}

void setup_stop(void) {
    if (!s_running) return;
    lock();
    s_closing = true;
    bool finish_trial = s_phase == SETUP_TRYING || s_phase == SETUP_DONE;
    unlock();
    s_running = false;
    if (s_http) httpd_stop(s_http);
    s_http = NULL;
    wifi_ap_stop();
    if (finish_trial) wifi_try(NULL, NULL);
    wipe(s_ap_pass, sizeof s_ap_pass);
    wipe(&s_req, sizeof s_req);
    wipe(&s_try, sizeof s_try);
    s_phase = SETUP_OFF;
    lock();
    if (s_mem) wipe(s_mem, sizeof *s_mem);
    free(s_mem);
    s_mem = NULL;
    s_nnets = s_qr_n[0] = s_qr_n[1] = 0;
    unlock();
    ESP_LOGI(TAG, "stopped");
}

bool setup_claim_idle_close(void) {
    lock();
    bool idle = !s_req.pending && s_phase != SETUP_TRYING && s_phase != SETUP_DONE;
    if (idle) s_closing = true;
    unlock();
    return idle;
}

bool setup_active(void) { return s_running; }

void setup_labels(setup_labels_t *out) {
    memset(out, 0, sizeof *out);
    if (!s_running) return;
    lock();
    memcpy(out->ap_ssid, s_ap_ssid, sizeof out->ap_ssid);
    memcpy(out->ap_pass, s_ap_pass, sizeof out->ap_pass);
    if (s_phase == SETUP_TRYING) memcpy(out->trying, s_try.ssid, sizeof out->trying);
    unlock();
}

const uint8_t *setup_qr(int *n, int *step) {
    int i = s_phase == SETUP_JOIN ? 0 : s_phase == SETUP_OPEN ? 1 : -1;
    setup_mem_t *mem = s_mem;
    if (i < 0 || !mem || !s_qr_n[i]) return NULL;
    *n = s_qr_n[i];
    *step = i + 1;
    return mem->qr[i];
}

static setup_error_t classify(uint8_t reason) {
    switch (reason) {
    case 2: case 15: case 202: case 204: case 205: case 210: return SETUP_ERR_PASSWORD;  // auth / 4-way handshake
    case 201: return SETUP_ERR_NOT_FOUND;
    default: return SETUP_ERR_OTHER;
    }
}

typedef struct { bool start_try, scan; } setup_work_t;

static setup_work_t take_pending_work(void) {
    setup_work_t work;
    lock();
    work.start_try = s_req.pending && s_phase != SETUP_TRYING && s_phase != SETUP_DONE;
    if (work.start_try) {
        s_try = s_req;
        wipe(&s_req, sizeof s_req);
        s_error = SETUP_ERR_NONE;
        set_phase(SETUP_TRYING);
    }
    work.scan = s_scan_wanted && s_phase != SETUP_TRYING && s_phase != SETUP_DONE;
    unlock();
    return work;
}

static void poll_join_or_open(void) {
    if (s_why == SETUP_LOST && wifi_sta_connected()) {
        ESP_LOGI(TAG, "saved network is back");
        setup_stop();
        return;
    }
    set_phase(wifi_ap_clients() > 0 ? SETUP_OPEN : SETUP_JOIN);
}

static void poll_trying(int64_t now) {
    if (wifi_sta_connected()) {
        esp_err_t err = settings_save_connection(s_try.ssid, s_try.pass, s_try.url);
        if (err == ESP_OK) err = setup_agent_save(&s_try.agent);
        lock();
        if (err == ESP_OK) {
            set_phase(SETUP_DONE);
        } else {
            s_error = SETUP_ERR_OTHER;
            set_phase(SETUP_FAILED);
        }
        wipe(&s_try.pass, sizeof s_try.pass);
        wipe(&s_try.agent, sizeof s_try.agent);
        unlock();
        if (err != ESP_OK) wifi_try(NULL, NULL);  // restore the saved profile after a failed NVS write
    } else if (now - s_phase_ms > TRY_TIMEOUT_MS || wifi_attempts() >= TRY_MAX_ATTEMPTS) {
        lock();
        s_error = wifi_attempts() ? classify(wifi_last_reason()) : SETUP_ERR_OTHER;
        set_phase(SETUP_FAILED);
        wipe(&s_try.pass, sizeof s_try.pass);
        wipe(&s_try.agent, sizeof s_try.agent);
        unlock();
        ESP_LOGW(TAG, "could not join '%s' (reason %d)", s_try.ssid, wifi_last_reason());
        wifi_try(NULL, NULL);  // back to the saved network, if any
    }
}

static void poll_failed(int64_t now) {
    if (now - s_phase_ms <= FAILED_SHOW_MS) return;
    lock();
    set_phase(wifi_ap_clients() > 0 ? SETUP_OPEN : SETUP_JOIN);
    unlock();
}

setup_phase_t setup_poll(void) {
    if (!s_running) return SETUP_OFF;
    int64_t now = now_ms();
    setup_work_t work = take_pending_work();
    if (work.start_try) {
        wifi_try(s_try.ssid, s_try.pass);
        return s_phase;
    }
    if (work.scan) scan_now();
    switch (s_phase) {
    case SETUP_JOIN: case SETUP_OPEN: poll_join_or_open(); break;
    case SETUP_TRYING: poll_trying(now); break;
    case SETUP_FAILED: poll_failed(now); break;
    default:
        break;
    }
    return s_phase;
}
