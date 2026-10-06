// Socket ownership and Noise crypto are serialized by the native Muse worker.
#include "muse_noise.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <new>
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <xplat/noise/core/ClientSession.h>
#include <xplat/noise/core/PsaCryptoBackend.h>
extern "C" {
#include "muse_link.h"
#include "muse_store.h"
}
using namespace musegadgets::noise::core;
namespace {
constexpr size_t RX_MAX = 17 * 1024, TX_MAX = 3 * 1024;
struct packet_t { uint8_t *bytes; size_t size, capacity; };
struct request_t { int64_t id; muse_link_req_cb callback; void *context; };
struct connection_t {
    PsaCryptoBackend crypto;
    ClientSession session{crypto};
    esp_websocket_client_handle_t socket{};
    QueueHandle_t incoming{};
    uint8_t *response{}, *service{}, *envelope{}, *rx{};
    size_t rx_size{}, rx_capacity{};
    std::atomic_size_t queued_bytes{};
    std::atomic_bool connected{}, failed{}, ready{};
    int64_t started{}, next_id{4};
    request_t requests[4]{};
};
connection_t *current;

ByteSpan span(uint8_t *data, size_t size) { return ByteSpan(data, size); }
void wipe_free(uint8_t *data, size_t size) {
    if (data) { muse_store_wipe(data, size); free(data); }
}
bool binary(const uint8_t *bytes, size_t size) {
    int written = esp_websocket_client_send_bin(current->socket, reinterpret_cast<const char *>(bytes),
        size, pdMS_TO_TICKS(1500));
    if (written == static_cast<int>(size)) return true;
    current->failed = true; return false;
}
bool flush(StatusWithSize status) {
    if (!status.ok()) { current->failed = true; return false; }
    while (current->session.HasOutboundWebSocketPayload()) {
        auto payload = current->session.WriteNextOutboundWebSocketPayload(span(current->service, TX_MAX));
        if (!payload.ok() || !binary(current->service, payload.size())) { current->failed = true; return false; }
    }
    return true;
}
bool allocate_rx(connection_t *context, const esp_websocket_event_data_t *event) {
    if (context->rx) return false;
    size_t capacity = event->fin ? event->payload_len : RX_MAX;
    if (!capacity || capacity > RX_MAX) return false;
    if (context->queued_bytes.fetch_add(capacity) + capacity > RX_MAX) {
        context->queued_bytes.fetch_sub(capacity); return false;
    }
    context->rx = static_cast<uint8_t *>(malloc(capacity)); context->rx_size = 0;
    context->rx_capacity = capacity;
    if (!context->rx) { context->queued_bytes.fetch_sub(capacity); return false; }
    return true;
}
void queue_rx(connection_t *context) {
    packet_t packet{context->rx, context->rx_size, context->rx_capacity};
    if (xQueueSend(context->incoming, &packet, 0) != pdTRUE) {
        wipe_free(context->rx, context->rx_capacity);
        context->queued_bytes.fetch_sub(context->rx_capacity); context->failed = true;
    }
    context->rx = nullptr; context->rx_size = context->rx_capacity = 0;
}
void receive_data(connection_t *context, const esp_websocket_event_data_t *event) {
    if (event->op_code != 2 && event->op_code != 0) return;
    if (event->payload_offset == 0 && event->op_code == 2 && !allocate_rx(context, event)) {
        context->failed = true; return;
    }
    if (!context->rx || event->data_len < 0 || context->rx_size + event->data_len > context->rx_capacity) {
        context->failed = true; return;
    }
    memcpy(context->rx + context->rx_size, event->data_ptr, event->data_len);
    context->rx_size += event->data_len;
    if (event->fin && event->payload_offset + event->data_len == event->payload_len) queue_rx(context);
}
void event_callback(void *argument, esp_event_base_t base, int32_t id, void *data) {
    (void)base; auto *context = static_cast<connection_t *>(argument);
    if (context->failed) return;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: context->connected = true; break;
    case WEBSOCKET_EVENT_DISCONNECTED: case WEBSOCKET_EVENT_ERROR: context->failed = true; break;
    case WEBSOCKET_EVENT_DATA: receive_data(context, static_cast<esp_websocket_event_data_t *>(data)); break;
    default: break;
    }
}
bool allocate(connection_t *context) {
    context->incoming = xQueueCreate(8, sizeof(packet_t));
    context->response = static_cast<uint8_t *>(malloc(RX_MAX));
    context->service = static_cast<uint8_t *>(malloc(TX_MAX));
    context->envelope = static_cast<uint8_t *>(malloc(TX_MAX));
    return context->incoming && context->response && context->service && context->envelope;
}
void route_frame(const DecodedServiceFrame &frame) {
    for (auto &request : current->requests) {
        if (!request.id || request.id != frame.stream_id) continue;
        int status = 0; ConstByteSpan body; bool end = false;
        switch (frame.kind) {
        case ServiceFrameKind::Response: status = frame.response.status; body = frame.response.body; end = frame.response.end_body; break;
        case ServiceFrameKind::BodyChunk: body = frame.body_chunk.data; end = frame.body_chunk.end_body; break;
        case ServiceFrameKind::Reset: status = -1; end = true; break;
        default: current->failed = true; return;
        }
        request.callback(request.context, status, body.data(), body.size(), end);
        if (end) request = {};
        return;
    }
}
void handshake(const packet_t &packet) {
    size_t extra = 0;
    auto status = current->session.ReadHandshakeMessage2(ConstByteSpan(packet.bytes, packet.size),
        span(current->response, RX_MAX), extra);
    if (!status.ok()) { current->failed = true; return; }
    auto output = current->session.WriteHandshakeMessage3(ConstByteSpan{}, span(current->service, TX_MAX));
    if (!output.ok() || !binary(current->service, output.size())) { current->failed = true; return; }
    current->ready = true;
}
// PSA and the IDF AES-GCM implementation support exact in-place decryption.
// Reuse the received packet as transport scratch; response reassembly stays separate.
void process_packet(const packet_t &packet) {
    if (!current->session.isEstablished()) { handshake(packet); return; }
    HeaderView headers[16];
    auto result = current->session.ProcessInboundWebSocketPayload(ConstByteSpan(packet.bytes, packet.size),
        span(packet.bytes, packet.capacity), span(current->response, RX_MAX), Span<HeaderView>(headers));
    if (!result.ok()) current->failed = true;
    else if (result.frame_status == InboundFrameStatus::Complete) route_frame(result.frame);
}
}

extern "C" const char *muse_noise_endpoint(void) { return "wss://hatch.metaaivm.com/v1/noise"; }

extern "C" bool muse_noise_start(const muse_vm_t *vm) {
    if (current || !vm || !vm->id[0] || !vm->token[0]) return false;
    current = new (std::nothrow) connection_t;
    if (!current) return false;
    if (!allocate(current)) { muse_noise_stop(); return false; }
    char uri[256]; snprintf(uri, sizeof uri, "%s?vm_id=%s", muse_noise_endpoint(), vm->id);
    char *headers = static_cast<char *>(malloc(strlen(vm->token) + 32));
    if (!headers) { muse_noise_stop(); return false; }
    snprintf(headers, strlen(vm->token) + 32, "Authorization: Bearer %s\r\n", vm->token);
    esp_websocket_client_config_t config{};
    config.uri = uri; config.headers = headers; config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048; config.task_stack = 5120; config.task_prio = 7;
    config.disable_auto_reconnect = true; config.network_timeout_ms = 12000;
    config.ping_interval_sec = 10; config.pingpong_timeout_sec = 30;
    current->socket = esp_websocket_client_init(&config);
    muse_store_wipe(headers, strlen(headers)); free(headers);
    if (!current->socket) { muse_noise_stop(); return false; }
    esp_err_t err = esp_websocket_register_events(current->socket, WEBSOCKET_EVENT_ANY, event_callback, current);
    if (err == ESP_OK) err = esp_websocket_client_start(current->socket);
    if (err != ESP_OK) { muse_noise_stop(); return false; }
    current->started = esp_timer_get_time(); return true;
}
extern "C" bool muse_noise_ready(void) { return current && current->ready && !current->failed; }
extern "C" bool muse_noise_failed(void) { return current && current->failed; }
extern "C" void muse_noise_tick(void) {
    if (!current || current->failed) return;
    if (!current->ready && esp_timer_get_time() - current->started > 20000000) { current->failed = true; return; }
    if (current->connected.exchange(false)) {
        auto hello = current->session.WriteHandshakeMessage1(span(current->service, TX_MAX));
        if (!hello.ok() || !binary(current->service, hello.size())) { current->failed = true; return; }
    }
    for (unsigned i = 0; i < 8 && !current->failed; i++) {
        packet_t packet;
        if (xQueueReceive(current->incoming, &packet, 0) != pdTRUE) break;
        process_packet(packet);
        wipe_free(packet.bytes, packet.capacity); current->queued_bytes.fetch_sub(packet.capacity);
    }
}
extern "C" void muse_noise_stop(void) {
    if (!current) return;
    current->failed = true;
    if (current->socket) { esp_websocket_client_stop(current->socket); esp_websocket_client_destroy(current->socket); }
    for (auto &request : current->requests)
        if (request.id) request.callback(request.context, -1, nullptr, 0, true);
    if (current->incoming) {
        packet_t packet;
        while (xQueueReceive(current->incoming, &packet, 0) == pdTRUE) wipe_free(packet.bytes, packet.capacity);
        vQueueDelete(current->incoming);
    }
    wipe_free(current->rx, current->rx_capacity); wipe_free(current->response, RX_MAX);
    wipe_free(current->service, TX_MAX); wipe_free(current->envelope, TX_MAX);
    delete current; current = nullptr;
}
extern "C" int64_t muse_link_req_open(const char *verb, const char *path, const char *const *headers,
        bool end, muse_link_req_cb callback, void *context) {
    if (!muse_noise_ready() || !callback) return 0;
    request_t *slot = nullptr;
    for (auto &request : current->requests) if (!request.id) { slot = &request; break; }
    if (!slot) return 0;
    HeaderView values[8]; size_t count = 0;
    while (headers && headers[2 * count]) {
        if (count == 8 || !headers[2 * count + 1]) return 0;
        values[count] = {StringView(headers[2 * count]), StringView(headers[2 * count + 1])}; count++;
    }
    int64_t id = current->next_id++;
    *slot = {id, callback, context};
    ApplicationRequestView request{StringView(verb), StringView(path), Span<const HeaderView>(values, count), {}, end};
    if (!flush(current->session.StartOutboundApplicationRequest(ServiceType::Daemon, id, request,
            span(current->service, TX_MAX), span(current->envelope, TX_MAX)))) { *slot = {}; return 0; }
    return id;
}
extern "C" bool muse_link_req_send(int64_t id, const void *data, size_t size, bool end, int wait_ms) {
    (void)wait_ms;
    if (!muse_noise_ready() || size > 2500) return false;
    bool found = false;
    for (const auto &request : current->requests) if (request.id == id) found = true;
    if (!found) return false;
    BodyChunkView chunk{ConstByteSpan(static_cast<const uint8_t *>(data), size), end};
    return flush(current->session.StartOutboundBodyChunk(ServiceType::Daemon, id, chunk,
        span(current->service, TX_MAX), span(current->envelope, TX_MAX)));
}
extern "C" void muse_link_req_cancel(int64_t id) {
    if (!current) return;
    for (auto &request : current->requests) {
        if (request.id != id || !id) continue;
        if (muse_noise_ready()) {
            ResetView reset{ResetCode::Cancelled, StringView("cancelled")};
            flush(current->session.StartOutboundReset(ServiceType::Daemon, id, reset,
                span(current->service, TX_MAX), span(current->envelope, TX_MAX)));
        }
        request = {}; return;
    }
}
extern "C" bool muse_link_req_ready(void) { return muse_noise_ready(); }
