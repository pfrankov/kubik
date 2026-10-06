#include <stdatomic.h>
#include <string.h>
#include "esp_transport.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

int __real_esp_transport_ws_send_raw(esp_transport_handle_t transport, ws_transport_opcodes_t opcode,
                                     const char *buffer, int bytes, int timeout_ms);
int __real_esp_transport_write(esp_transport_handle_t transport, const char *buffer, int bytes, int timeout_ms);

// The SDK writes a masked header then its body in separate TLS records. A Live
// frame fits in one 1 KB record; packing those writes avoids a tiny TCP segment
// waiting for an ACK while the client lock prevents incoming speech from being read.
// Keep the SDK's framing/masking, TLS, pinning, parser and common client lock.
static _Atomic(TaskHandle_t) s_owner;
static esp_transport_handle_t s_parent;
static int s_payload, s_header;
static char s_record[1024];

int __wrap_esp_transport_write(esp_transport_handle_t transport, const char *buffer, int bytes, int timeout_ms) {
    TaskHandle_t owner = atomic_load(&s_owner);
    if (!owner || owner != xTaskGetCurrentTaskHandle())
        return __real_esp_transport_write(transport, buffer, bytes, timeout_ms);
    if (!s_header) {
        if (bytes < 6 || bytes > 14) return -1;
        if (!s_payload) return __real_esp_transport_write(transport, buffer, bytes, timeout_ms);
        s_parent = transport; s_header = bytes;
        memcpy(s_record, buffer, bytes);
        return bytes; // The body call below writes this header with its first bytes.
    }
    if (transport != s_parent || bytes != s_payload) return -1;
    int offset = 0, prefix = s_header;
    while (offset < bytes) {
        int count = bytes - offset;
        if (count > (int)sizeof s_record - prefix) count = sizeof s_record - prefix;
        memcpy(s_record + prefix, buffer + offset, count);
        int total = prefix + count;
        if (__real_esp_transport_write(transport, s_record, total, timeout_ms) != total) return -1;
        offset += count; prefix = 0;
    }
    return bytes;
}

// One WebSocket client serializes send_raw under the SDK lock. Task ownership
// confines the nested write interception to this call; other transports pass through.
int __wrap_esp_transport_ws_send_raw(esp_transport_handle_t transport, ws_transport_opcodes_t opcode,
                                     const char *buffer, int bytes, int timeout_ms) {
    int socket = esp_transport_get_socket(transport), enabled = 1;
    if (socket < 0 || setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled)) return -1;
    atomic_store(&s_owner, xTaskGetCurrentTaskHandle()); s_parent = NULL; s_payload = bytes; s_header = 0;
    int result = __real_esp_transport_ws_send_raw(transport, opcode, buffer, bytes, timeout_ms);
    atomic_store(&s_owner, NULL); s_parent = NULL; s_header = 0;
    memset(s_record, 0, sizeof s_record);
    return result;
}
