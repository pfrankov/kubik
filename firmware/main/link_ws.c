#include "link_ws.h"
#include "app_wake.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "heap_probe.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"

static link_ws_context_t *s_context;
static size_t s_expected, s_received;

void link_ws_bind(link_ws_context_t *context) { s_context = context; }

static void ws_release_message(void) {
    free(*s_context->rx);
    *s_context->rx = NULL;
    s_expected = s_received = 0;
}

static void ws_connected_locked(void) {
    link_ws_context_t *c = s_context;
    ws_release_message();
    *c->connected = true;
    *c->failed = false;
    hp_mark("ws connected");
}

static void ws_error_locked(esp_websocket_event_data_t *d) {
    link_ws_context_t *c = s_context;
    ESP_LOGW(c->tag, "websocket error type=%d http=%d", d ? (int)d->error_handle.error_type : -1,
             d ? d->error_handle.esp_ws_handshake_status_code : 0);
    hp_mark("ws error");
    if (d && d->error_handle.error_type == WEBSOCKET_ERROR_TYPE_HANDSHAKE)
        atomic_store(c->problem, LINK_NO_PLUGIN);
    else if (d && d->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT && !*c->connected &&
             atomic_load(c->problem) != LINK_REFUSED && atomic_load(c->problem) != LINK_KEY_CHANGED)
        atomic_store(c->problem, LINK_NO_SERVER);  // a changed key stays shown: it fails TLS too
}

static void ws_disconnected_locked(void) {
    link_ws_context_t *c = s_context;
    if (*c->ready) *c->down_ms = c->now_ms();
    *c->connected = *c->ready = *c->pairing = false;
    *c->failed = true;
    ws_release_message();
    if (atomic_load(c->via) == c->via_wifi) c->set_via(c->via_none);
}

static bool ws_close_frame(esp_websocket_event_data_t *d) {
    return d && d->op_code == 0x08 && d->data_len >= 2;
}

static bool ws_fragment_lengths_valid(esp_websocket_event_data_t *d) {
    link_ws_context_t *c = s_context;
    if (d->payload_len < 0 || d->payload_offset < 0 || d->data_len < 0) return false;
    if (d->payload_len > LINK_WS_RX_MAX || (*c->rx_op == 0x1 && d->payload_len > LINK_JSON_MAX)) return false;
    if (d->payload_offset > d->payload_len) return false;
    return d->data_len <= d->payload_len - d->payload_offset;
}

static bool ws_message_storage(esp_websocket_event_data_t *d) {
    link_ws_context_t *c = s_context;
    if (d->payload_offset == 0) {
        ws_release_message();
        s_expected = (size_t)d->payload_len;
        *c->rx = malloc(s_expected + 1);
        if (!*c->rx) { ws_disconnected_locked(); return false; }
    }
    if (!*c->rx || s_expected != (size_t)d->payload_len || s_received != (size_t)d->payload_offset) {
        ws_release_message(); return false;
    }
    return true;
}

static bool ws_receive_fragment(esp_websocket_event_data_t *d, size_t *len) {
    link_ws_context_t *c = s_context;
    if (!*c->connected || !d) return false;
    if (d->op_code != 0x1 && d->op_code != 0x2 && d->op_code != 0x0) return false;
    if (d->payload_offset == 0 && d->op_code != 0) *c->rx_op = d->op_code;
    if (!ws_fragment_lengths_valid(d)) { ws_release_message(); return false; }
    if (!ws_message_storage(d)) return false;
    memcpy(*c->rx + d->payload_offset, d->data_ptr, d->data_len);
    s_received += (size_t)d->data_len;
    if (d->payload_offset + d->data_len < d->payload_len) return false;
    *len = (size_t)d->payload_len;
    return true;
}

static void ws_text_message_locked(size_t len) {
    link_ws_context_t *c = s_context;
    (*c->rx)[len] = 0;
    if (!*c->ready && *c->hello_sent && c->is_welcome(*c->rx, len)) {
        *c->ready = true;
        app_wake_transport_busy(false);
        *c->pairing = false;
        *c->usb_ready = false;
        atomic_store(c->problem, LINK_OK);
        c->set_via(c->via_wifi);
    } else if (!*c->ready && *c->hello_sent) c->handshake(*c->rx, len, false, 0);
    if (*c->ready && atomic_load(c->via) == c->via_wifi && c->handlers->on_json) c->handlers->on_json(*c->rx, len);
}

static void ws_binary_message_locked(const uint8_t *data, size_t len) {
    link_ws_context_t *c = s_context;
    if (len >= 2 && *c->ready && atomic_load(c->via) == c->via_wifi && c->handlers->on_audio)
        c->handlers->on_audio(data[0], data[1], data + 2, len - 2);
}

static bool ws_complete_binary(esp_websocket_event_data_t *d) {
    return d && *s_context->connected && d->fin && d->op_code == 0x2 && d->payload_offset == 0 &&
           d->payload_len >= 2 && d->payload_len <= LINK_WS_RX_MAX &&
           d->data_len == d->payload_len && d->data_ptr;
}

static void ws_data_locked(esp_websocket_event_data_t *d) {
    link_ws_context_t *c = s_context;
    if (ws_close_frame(d)) {
        int code = (uint8_t)d->data_ptr[0] << 8 | (uint8_t)d->data_ptr[1];
        ESP_LOGW(c->tag, "server closed the session (%d)", code);
        if (code == 4001) atomic_store(c->problem, LINK_REFUSED);
        return;
    }
    if (ws_complete_binary(d)) {
        // The SDK owns these bytes until this synchronous callback returns.
        // Normal 100 ms speech fits its RX buffer; only fragmented messages need storage.
        ws_release_message();
        ws_binary_message_locked((const uint8_t *)d->data_ptr, (size_t)d->data_len);
        return;
    }
    size_t len;
    if (!ws_receive_fragment(d, &len)) return;
    if (*c->rx_op == 0x1) ws_text_message_locked(len);
    else if (*c->rx_op == 0x2) ws_binary_message_locked((const uint8_t *)*c->rx, len);
    ws_release_message();
}

void link_ws_dispatch(void *arg, int32_t id, void *data) {
    link_ws_context_t *c = s_context;
    esp_websocket_event_data_t *d = data;
    c->lock_route();
    if (arg != c->active_ws) goto done;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: ws_connected_locked(); break;
    case WEBSOCKET_EVENT_ERROR:
        ws_error_locked(d);
        // fall through
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_FINISH: ws_disconnected_locked(); break;
    case WEBSOCKET_EVENT_DATA: ws_data_locked(d); break;
    default: break;
    }
done:
    c->unlock_route();
}
