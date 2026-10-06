#include "link.h"
#include "link_mic.h"
#include "muse_backend.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "link_discover.h"
#include "link_hello.h"
#include "link_pin.h"
#include "link_poke.h"
#include "link_usb.h"
#include "link_ws.h"
#include "app_wake.h"
#include "heap_probe.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"
#include "wifi.h"
static const char *TAG = "link";
#define USB_PROBE_MS 8000
#define WSS_ATTEMPT_MS 22000
#define WS_SEND_MS 1500
// TLS RX shares priority with speaker/mic delivery (7), below display (8).
// The receiver fills a 900 ms ADPCM ring; host credit limits in-flight speech to 800 ms.
#define WS_TASK_PRIO 7
// Wi-Fi is the primary route; USB is offered (the hello that makes the bridge open its upstream) only after
// Wi-Fi had this long to come up at boot, or to come back after a session it lost.
#define USB_BOOT_GRACE_MS 12000
#define USB_DROP_GRACE_MS 3000
typedef enum { VIA_NONE=0, VIA_USB, VIA_WIFI, VIA_MUSE } via_t;
typedef enum { USB_DISABLED, USB_PROBING, USB_READY, USB_UNAVAILABLE } usb_status_t;
static link_handlers_t s_h;
static atomic_int s_via;
static atomic_uint s_session;
static atomic_bool s_host_seen, s_wifi_allowed = true;
static atomic_uint s_host_ms, s_wifi_pause_until_ms;
// Route transitions and callback delivery are serialized; network sends and
// client destruction have a separate lock so WS callbacks cannot deadlock them.
static SemaphoreHandle_t s_route_mtx, s_ws_io_mtx;
static uint32_t s_usb_epoch, s_probe_epoch, s_probe_ms;
static usb_status_t s_usb_status;
static bool s_usb_ready;
static esp_websocket_client_handle_t s_ws;
static bool s_ws_connected, s_ws_ready, s_ws_failed, s_ws_hello_sent;
static uint32_t s_ws_started_ms, s_ws_attempt_ms, s_ws_down_ms;
static char *s_ws_rx;
static char s_endpoint[160];
static uint8_t s_ws_rx_op;
// Handshake: the server's challenge waits here for the manager task to sign it
// (the signature takes a moment and a deeper stack than the socket callbacks).
static char s_nonce[68];
static bool s_nonce_usb, s_ws_pairing;
static uint32_t s_nonce_epoch;
static atomic_int s_problem;
static uint32_t now_ms(void);
static void handshake_frame(const char *text, size_t len, bool usb, uint32_t epoch);
static void ws_route_lock(void);
static void ws_route_unlock(void);
static void ws_set_via(int via);
static link_ws_context_t s_ws_context = {
    .tag = "link",
    .connected = &s_ws_connected, .ready = &s_ws_ready, .failed = &s_ws_failed,
    .hello_sent = &s_ws_hello_sent, .pairing = &s_ws_pairing, .usb_ready = &s_usb_ready,
    .down_ms = &s_ws_down_ms, .rx = &s_ws_rx, .rx_op = &s_ws_rx_op,
    .via = &s_via, .problem = &s_problem, .handlers = &s_h,
    .lock_route = ws_route_lock, .unlock_route = ws_route_unlock,
    .now_ms = now_ms, .is_welcome = link_is_welcome, .handshake = handshake_frame, .set_via = ws_set_via,
    .via_none = VIA_NONE, .via_wifi = VIA_WIFI,
};
link_problem_t link_last_problem(void) { return (link_problem_t)atomic_load(&s_problem); }
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
bool link_usb_host_present(void) {
    return atomic_load(&s_host_seen) && (uint32_t)(now_ms() - atomic_load(&s_host_ms)) < 2500;
}
static void set_via_locked(via_t v) {
    via_t old = atomic_load(&s_via);
    if (old == v) return;
    if (atomic_fetch_add(&s_session, 1) == UINT32_MAX) atomic_fetch_add(&s_session, 1);
    atomic_store(&s_via, v);
    if (v == VIA_WIFI) s_probe_epoch = 0; // permit same-epoch cached USB welcome after Wi-Fi loss
    if (old != VIA_NONE && s_h.on_link) s_h.on_link(false);
    if (v != VIA_NONE && s_h.on_link) s_h.on_link(true);
    ESP_LOGI(TAG, "link: %s", v == VIA_USB ? "usb" : v == VIA_WIFI ? "wifi" : v == VIA_MUSE ? "muse" : "none");
}
static void handshake_challenge(cJSON *j, bool usb, uint32_t epoch) {
    const char *nonce = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "nonce"));
    if (!nonce || strlen(nonce) >= sizeof s_nonce) return;
    snprintf(s_nonce, sizeof s_nonce, "%s", nonce);
    s_nonce_usb = usb;
    s_nonce_epoch = epoch;
}
static void handshake_pair(cJSON *j, bool usb) {
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "code"));
    if (!code || !code[0] || strlen(code) > 16) return;
    if (!usb) s_ws_pairing = true;
    atomic_store(&s_problem, LINK_PAIRING);
    if (s_h.on_pair) s_h.on_pair(code);
}
static void handshake_frame(const char *text, size_t len, bool usb, uint32_t epoch) {
    cJSON *j = cJSON_ParseWithLength(text, len);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "t"));
    if (!type) { cJSON_Delete(j); return; }
    if (!strcmp(type, "challenge")) handshake_challenge(j, usb, epoch);
    else if (!strcmp(type, "pair")) handshake_pair(j, usb);
    else if (!strcmp(type, "error")) {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "code"));
        if (code && !strcmp(code, "unauthorized")) atomic_store(&s_problem, LINK_REFUSED);
    }
    cJSON_Delete(j);
}
static void epoch_bytes(uint8_t p[4], uint32_t epoch) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(epoch >> (8*i));
}
static uint32_t read_epoch(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool usb_probe_pending_locked(uint32_t now) {
    return link_usb_host_present() && !s_usb_ready && s_probe_epoch == s_usb_epoch &&
        (s_usb_status == USB_PROBING || s_usb_status == USB_READY) &&
        (uint32_t)(now - s_probe_ms) < USB_PROBE_MS;
}
static bool parse_usb_status(const char *text, usb_status_t *status) {
    if (!text) return false;
    if (!strcmp(text, "disabled")) *status = USB_DISABLED;
    else if (!strcmp(text, "probing")) *status = USB_PROBING;
    else if (!strcmp(text, "ready")) *status = USB_READY;
    else if (!strcmp(text, "unavailable")) *status = USB_UNAVAILABLE;
    else return false;
    return true;
}
static bool decode_host_hello(const char *text, size_t len, usb_status_t *status, uint32_t *epoch) {
    cJSON *j = cJSON_ParseWithLength(text, len);
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "v");
    cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "epoch");
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "status"));
    bool valid = cJSON_IsNumber(v) && v->valuedouble == 2 && cJSON_IsNumber(e) &&
        e->valuedouble >= 1 && e->valuedouble <= 4294967295.0 &&
        e->valuedouble == (double)(uint32_t)e->valuedouble;
    bool ok = valid && parse_usb_status(name, status);
    if (ok) *epoch = (uint32_t)e->valuedouble;
    cJSON_Delete(j);
    return ok;
}
static void host_hello(const char *text, size_t len) {
    usb_status_t status;
    uint32_t epoch;
    if (!decode_host_hello(text, len, &status, &epoch)) return;
    xSemaphoreTake(s_route_mtx, portMAX_DELAY);
    atomic_store(&s_host_ms, now_ms());
    atomic_store(&s_host_seen, true);
    if (epoch != s_usb_epoch || status != USB_READY) {
        s_usb_ready = false;
        if (atomic_load(&s_via) == VIA_USB) set_via_locked(VIA_NONE);
    }
    if (epoch != s_usb_epoch) s_probe_epoch = 0;
    s_usb_epoch = epoch;
    s_usb_status = status;
    xSemaphoreGive(s_route_mtx);
}
static bool usb_payload_valid(uint8_t type, size_t len) {
    if (type == F_JSON && len > LINK_JSON_MAX) return false;
    if (type == F_AUDIO && len > LINK_WS_RX_MAX) return false;
    return true;
}
static bool usb_handshake_pending(uint8_t type, const char *data, size_t len) {
    if (type != F_JSON || s_usb_ready || s_ws_ready) return false;
    if (s_usb_status != USB_PROBING && s_usb_status != USB_READY) return false;
    return !link_is_welcome(data, len);
}
static bool usb_welcome_pending(uint8_t type, const char *data, size_t len) {
    return type == F_JSON && !s_usb_ready && !s_ws_ready && link_is_welcome(data, len);
}
static void dispatch_usb_input(uint8_t type, uint8_t *data, size_t len) {
    if (type == F_JSON && s_h.on_json) s_h.on_json((const char *)data, len);
    else if (type == F_AUDIO && len >= 2 && s_h.on_audio) s_h.on_audio(data[0], data[1], data + 2, len - 2);
}
static void usb_routed_input(uint8_t type, uint8_t *data, size_t len) {
    if (muse_backend_selected()) return;
    if (len < LINK_USB_EPOCH_BYTES) return;
    uint32_t epoch = read_epoch(data);
    data += LINK_USB_EPOCH_BYTES; len -= LINK_USB_EPOCH_BYTES;
    if (!usb_payload_valid(type, len)) return;
    xSemaphoreTake(s_route_mtx, portMAX_DELAY);
    if (!link_usb_host_present() || epoch != s_usb_epoch) goto done;
    if (type == F_BIND) {
        link_pin_usb_bind(epoch, (const char *)data, len);
        goto done;
    }
    // The handshake (challenge, pairing code) runs while the bridge is still
    // probing; the welcome and everything after it need a ready bridge.
    if (usb_handshake_pending(type, (const char *)data, len)) {
        handshake_frame((const char *)data, len, true, epoch);
        goto done;
    }
    if (s_usb_status != USB_READY) goto done;
    if (usb_welcome_pending(type, (const char *)data, len)) {
        s_usb_ready = true;
        atomic_store(&s_problem, LINK_OK);
        // The authenticated welcome promotes the route before app callbacks,
        // including when it arrives between the manager's 200ms ticks.
        set_via_locked(VIA_USB);
    }
    if (s_usb_ready && atomic_load(&s_via) == VIA_USB) dispatch_usb_input(type, data, len);
done:
    xSemaphoreGive(s_route_mtx);
}
static void ws_route_lock(void) { xSemaphoreTake(s_route_mtx, portMAX_DELAY); }
static void ws_route_unlock(void) { xSemaphoreGive(s_route_mtx); }
static void ws_set_via(int via) { set_via_locked((via_t)via); }
static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)base;
    link_ws_dispatch(arg, id, data);
}
static void ws_stop(void) {
    xSemaphoreTake(s_ws_io_mtx, portMAX_DELAY);
    xSemaphoreTake(s_route_mtx, portMAX_DELAY);
    esp_websocket_client_handle_t ws = s_ws;
    s_ws = NULL;
    s_ws_context.active_ws = NULL;
    char *rx = s_ws_rx;
    s_ws_rx = NULL;
    if (s_ws_ready) s_ws_down_ms = now_ms();
    s_ws_connected = s_ws_ready = s_ws_failed = s_ws_hello_sent = s_ws_pairing = false;
    if (!s_nonce_usb) s_nonce[0] = 0;
    if (atomic_load(&s_via) == VIA_WIFI) set_via_locked(VIA_NONE);
    xSemaphoreGive(s_route_mtx);
    if (ws) {
        // Disconnected/start-failed clients can reject stop; destroy still owns
        // and must release the allocation. Never destroy from an event callback.
        hp_mark("ws stop");
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
        hp_mark("ws destroyed");
    app_wake_transport_busy(false);
    }
    free(rx);
    xSemaphoreGive(s_ws_io_mtx);
}
static bool ws_start(const char *uri) {
    if (strncmp(uri, "wss://", 6)) return false;
    bool pinned = link_server_mode(g_settings.server_url) <= SERVER_LAN_FIXED;
    app_wake_transport_busy(true);
    hp_mark("ws start");
    esp_websocket_client_config_t cfg = {
        .uri = uri, .buffer_size = 2048,
        .disable_auto_reconnect = true, .network_timeout_ms = 15000,
        // Unanswered pings mean a dead route (e.g. the server dropped us): reconnect.
        .ping_interval_sec = 10, .pingpong_timeout_sec = 30,
        .task_stack = 5120, .task_prio = WS_TASK_PRIO,
        // LAN mode pins the plugin's own key; a public address needs a CA.
        .crt_bundle_attach = pinned ? link_pin_attach : esp_crt_bundle_attach,
    };
    xSemaphoreTake(s_ws_io_mtx, portMAX_DELAY);
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    if (!ws) { xSemaphoreGive(s_ws_io_mtx); ESP_LOGW(TAG, "websocket allocation failed"); app_wake_transport_busy(false); return false; }
    xSemaphoreTake(s_route_mtx, portMAX_DELAY);
    snprintf(s_endpoint, sizeof s_endpoint, "%s", uri);
    s_ws = ws;
    s_ws_context.active_ws = ws;
    s_ws_connected = s_ws_ready = s_ws_failed = s_ws_hello_sent = false;
    s_ws_started_ms = now_ms();
    s_ws_attempt_ms = WSS_ATTEMPT_MS;
    link_pin_begin_wifi();
    xSemaphoreGive(s_route_mtx);
    esp_err_t err = esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, ws);
    if (err == ESP_OK) err = esp_websocket_client_start(ws);
    xSemaphoreGive(s_ws_io_mtx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "websocket start failed: %s", esp_err_to_name(err));
        ws_stop(); return false;
    }
    hp_mark("ws connecting");
    return true;
}
static bool ws_send(const void *data, size_t len, bool binary, bool handshake, uint32_t session) {
    xSemaphoreTake(s_ws_io_mtx, portMAX_DELAY);
    xSemaphoreTake(s_route_mtx, portMAX_DELAY);
    esp_websocket_client_handle_t ws = s_ws;
    bool allowed = ws && s_ws_connected && (!session || session == atomic_load(&s_session)) &&
        (handshake || (s_ws_ready && atomic_load(&s_via) == VIA_WIFI));
    xSemaphoreGive(s_route_mtx);
    int n = -1;
    // A failed write makes the client drop the whole connection, so give a slow uplink time.
    if (allowed) n = binary ? esp_websocket_client_send_bin(ws, data, len, pdMS_TO_TICKS(WS_SEND_MS))
                            : esp_websocket_client_send_text(ws, data, len, pdMS_TO_TICKS(WS_SEND_MS));
    xSemaphoreGive(s_ws_io_mtx);
    return n == (int)len;
}
void link_endpoint(char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    if (!s_route_mtx) return;
    xSemaphoreTake(s_route_mtx, portMAX_DELAY);
    if (atomic_load(&s_via) == VIA_WIFI) snprintf(out, cap, "%s", s_endpoint);
    xSemaphoreGive(s_route_mtx);
}
const char *link_via(void) { int v = atomic_load(&s_via); return v == VIA_USB ? "usb" : v == VIA_WIFI || v == VIA_MUSE ? "wifi" : "none"; }
uint32_t link_session(void) { return atomic_load(&s_session); }
static bool send_json_session(const char *json, uint32_t session) {
    if (strlen(json) > LINK_JSON_MAX) return false;
    xSemaphoreTake(s_route_mtx, portMAX_DELAY);
    via_t via = !session || session == atomic_load(&s_session) ? atomic_load(&s_via) : VIA_NONE;
    uint32_t epoch = s_usb_epoch, current = atomic_load(&s_session);
    xSemaphoreGive(s_route_mtx);
    if (via == VIA_MUSE) return muse_backend_json(json, current);
    if (via == VIA_USB) { uint8_t e[4]; epoch_bytes(e, epoch); return link_usb_frame(F_JSON, e, 4, json, strlen(json)); }
    return via == VIA_WIFI && ws_send(json, strlen(json), false, false, session);
}
bool link_send_json(const char *json) { return send_json_session(json, 0); }
bool link_send_json_in_session(const char *json, uint32_t session) { return session && send_json_session(json, session); }
static bool send_mic_session(uint8_t turn, const int16_t *pcm, size_t len, uint32_t session, const uint8_t *ima) {
    xSemaphoreTake(s_route_mtx, portMAX_DELAY);
    uint32_t current = atomic_load(&s_session);
    via_t via = session == current ? atomic_load(&s_via) : VIA_NONE;
    uint32_t epoch = s_usb_epoch;
    xSemaphoreGive(s_route_mtx);
    if (via == VIA_NONE) return false;
    if (via == VIA_MUSE) return muse_backend_pcm(turn, pcm, len, ima, current);
    size_t bytes = 0;
    const uint8_t *frame = pcm ? link_mic_encode(turn, current, pcm, len, ima, &bytes)
                               : link_mic_duplex_encode(turn, ima, &bytes);
    if (!frame) return false;
    if (via == VIA_USB) {
        uint8_t header[4]; epoch_bytes(header, epoch);
        return link_usb_frame(F_AUDIO, header, sizeof header, frame, bytes);
    }
    return ws_send(frame, bytes, true, false, current);
}
bool link_send_mic_in_session(uint8_t turn, const int16_t *pcm, size_t len, uint32_t session, const uint8_t *ima) {
    return session && send_mic_session(turn, pcm, len, session, ima);
}
bool link_send_duplex_in_session(uint8_t turn, const uint8_t *ima, uint32_t session) {
    return session && send_mic_session(turn, NULL, 0, session, ima);
}
void link_set_wifi_allowed(bool allowed) {
    atomic_store(&s_wifi_allowed, allowed);
    muse_backend_allow(allowed);
    if (!allowed) ws_stop();  // now, not at the manager's next turn: the setup that follows needs the memory the socket holds
}
bool link_wifi_stopped(void) { return muse_backend_wifi_stopped(); }
void link_pause_wifi(uint32_t ms) { atomic_store(&s_wifi_pause_until_ms, now_ms() + ms); }
typedef struct {
    uint32_t now, epoch, nonce_epoch;
    bool wifi_ok, probe, pending, has_ws, failed, ready, hello, nonce_usb, bound;
    char nonce[sizeof s_nonce], bind[LINK_BIND_MAX];
} manager_state_t;
typedef struct { uint32_t boot, next_ws, backoff; bool was_ready; } manager_retry_t;
static bool manager_wifi_ok(void) {
    return atomic_load(&s_wifi_allowed) && g_settings.wifi_ssid[0] &&
        (int32_t)(now_ms() - atomic_load(&s_wifi_pause_until_ms)) >= 0;
}
// Called with the route mutex held; USB is offered only when Wi-Fi has no session in flight.
static bool manager_usb_probe_locked(manager_state_t *s) {
    bool wanted = !s_ws_ready && !(s_ws_connected && s_ws_hello_sent) &&
        (!s->wifi_ok || (int32_t)(s->now - s_ws_down_ms) >= USB_DROP_GRACE_MS);
    if (!link_usb_host_present()) {
        s_usb_ready = false; s_probe_epoch = 0;
        if (atomic_load(&s_via) == VIA_USB) set_via_locked(VIA_NONE);
    } else if (wanted && !s_usb_ready && s_usb_epoch != s_probe_epoch &&
               (s_usb_status == USB_PROBING || s_usb_status == USB_READY)) {
        s->epoch = s_usb_epoch;
        s_probe_epoch = s->epoch; s_probe_ms = s->now; s->probe = true;
    }
    return usb_probe_pending_locked(s->now);
}
// The server key: bind for a pending challenge, pin after a welcome, report a changed key.
static void manager_pin_locked(manager_state_t *s) {
    if (s->nonce[0]) s->bound = link_pin_bind(s->nonce_usb, s->nonce_epoch, s->bind);
    if (s_ws_ready) link_pin_keep(false, 0);
    if (s_usb_ready) link_pin_keep(true, s_usb_epoch);
    if (link_pin_take_mismatch()) atomic_store(&s_problem, LINK_KEY_CHANGED);
}
static void manager_snapshot(manager_state_t *s) {
    xSemaphoreTake(s_route_mtx, portMAX_DELAY);
    s->pending = manager_usb_probe_locked(s);
    s->has_ws = s_ws != NULL;
    // Pairing waits for the owner's approval; the server controls that deadline.
    s->failed = s_ws_failed || (s->has_ws && !s_ws_ready && !s_ws_pairing &&
        (uint32_t)(s->now - s_ws_started_ms) >= s_ws_attempt_ms);
    s->nonce_usb = s_nonce_usb; s->nonce_epoch = s_nonce_epoch;
    snprintf(s->nonce, sizeof s->nonce, "%s", s_nonce); s_nonce[0] = 0;
    manager_pin_locked(s);
    s->ready = s_ws_ready;
    s->hello = s->has_ws && s_ws_connected && !s_ws_hello_sent && !s->pending;
    if (s->hello) s_ws_hello_sent = true;
    xSemaphoreGive(s_route_mtx);
}
static void manager_mark_ws_failed(manager_state_t *s) {
    xSemaphoreTake(s_route_mtx, portMAX_DELAY); s_ws_failed = true; xSemaphoreGive(s_route_mtx);
    s->failed = true;
}
static void manager_send_hello(manager_state_t *s) {
    if (!s->probe && !s->hello) return;
    char text[1024]; bool made = link_make_hello(text, sizeof text);
    if (s->probe) {
        uint8_t e[4]; epoch_bytes(e, s->epoch);
        if (!made || !link_usb_frame(F_JSON, e, 4, text, made ? strlen(text) : 0))
            ESP_LOGW(TAG, "USB probe send failed; waiting for next bounded host probe");
    }
    if (s->hello && (!made || !ws_send(text, strlen(text), false, true, 0))) manager_mark_ws_failed(s);
}
static void manager_send_challenge(manager_state_t *s) {
    if (!s->nonce[0]) return;
    char auth[160];
    // Without a binding (none reported, or a key that is not the pinned one) there is no answer.
    bool ok = s->bound && link_make_auth(s->nonce, s->bind, auth, sizeof auth);
    if (s->nonce_usb) {
        uint8_t e[4]; epoch_bytes(e, s->nonce_epoch);
        if (ok) link_usb_frame(F_JSON, e, 4, auth, strlen(auth));
    } else if (!ok || !ws_send(auth, strlen(auth), false, true, 0)) manager_mark_ws_failed(s);
}

static void manager_backoff(manager_retry_t *r) {
    r->backoff = r->backoff < 5000 ? r->backoff * 2 : 10000;
}

static void manager_retry_failed(manager_state_t *s, manager_retry_t *r) {
    ws_stop(); r->next_ws = s->now + (r->was_ready ? 500 : r->backoff);
    if (!r->was_ready) manager_backoff(r);
    r->was_ready = false;
}
static bool manager_ws_due(manager_state_t *s, manager_retry_t *r) {
    return !s->has_ws && !s->pending && (uint32_t)(s->now - r->boot) >= 3000 &&
        ((int32_t)(s->now - r->next_ws) >= 0) && wifi_link_attempt_allowed() &&
        (strncmp(g_settings.server_url, "wss://", 6) || wifi_time_ready());
}

static void manager_stop_wifi(manager_state_t *s) {
    if (s->has_ws) ws_stop();
    link_target_stop();
}
// LAN discovery takes a few ticks; a server that is not found counts as a failed attempt.
static void manager_connect(manager_state_t *s, manager_retry_t *r) {
    if (!manager_wifi_ok() || !wifi_radio_started()) return;
    char uri[160];
    link_target_t target = link_target(s->now, uri, sizeof uri);
    if (target == TARGET_WAITING) return;
    wifi_link_attempt_started();
    if (target == TARGET_READY && ws_start(uri)) {
        if (!manager_wifi_ok() || !wifi_radio_started()) ws_stop();
        return;
    }
    link_problem_t problem = atomic_load(&s_problem);
    if (target == TARGET_NOT_FOUND && problem != LINK_REFUSED && problem != LINK_KEY_CHANGED)
        atomic_store(&s_problem, LINK_NO_SERVER);
    r->next_ws = s->now + r->backoff; manager_backoff(r);
}

static void manager_wifi_step(manager_state_t *s, manager_retry_t *r) {
    if (!s->wifi_ok) { manager_stop_wifi(s); return; }
    // Keep Wi-Fi alive while USB carries traffic so it can take over seamlessly.
    wifi_start_sta();
    if (!wifi_sta_connected()) { manager_stop_wifi(s); return; }
    if (s->failed) { manager_retry_failed(s, r); return; }
    if (s->ready) { r->backoff = 2000; r->was_ready = true; return; }
    if (manager_ws_due(s, r)) manager_connect(s, r);
}

static void manager_task(void *arg) {
    (void)arg;
    manager_retry_t retry = {.boot = now_ms(), .next_ws = now_ms() + 3000, .backoff = 2000};
    s_ws_down_ms = retry.boot + USB_BOOT_GRACE_MS - USB_DROP_GRACE_MS;  // boot: the longer grace
    for (;;) {
        if (muse_backend_selected()) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        manager_state_t s = {.now = now_ms(), .wifi_ok = manager_wifi_ok()};
        manager_snapshot(&s); manager_send_hello(&s); manager_send_challenge(&s); manager_wifi_step(&s, &retry);
        link_poke_tick(s.now);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void muse_route(bool ready) {
    xSemaphoreTake(s_route_mtx, portMAX_DELAY);
    set_via_locked(ready ? VIA_MUSE : VIA_NONE);
    xSemaphoreGive(s_route_mtx);
}
void link_init(const link_handlers_t *h) {
    s_h = *h;
    s_route_mtx = xSemaphoreCreateMutex();
    s_ws_io_mtx = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_route_mtx && s_ws_io_mtx ? ESP_OK : ESP_ERR_NO_MEM);
    link_ws_bind(&s_ws_context);
    link_usb_handlers_t usb_handlers = {
        .on_host_hello = host_hello,
        .on_config = s_h.on_config,
        .on_routed = usb_routed_input,
    };
    link_usb_start(&usb_handlers);
    esp_log_set_vprintf(link_usb_log_vprintf);
    muse_backend_init(&s_h, muse_route);
    ESP_ERROR_CHECK(xTaskCreate(manager_task, "link", 5632, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
