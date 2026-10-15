// Exercise the pinned SDK allocation + send loop, not a copied implementation.
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define CONFIG_ESP_WS_CLIENT_ENABLE_DYNAMIC_BUFFER 1
#define CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK 1
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_WS_CLIENT_MEM_CHECK(tag, value, action) do { if (!(value)) { action; } } while (0)
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 1
#define CLOSE_FRAME_SENT_BIT 1
#define WS_TRANSPORT_OPCODES_FIN 128
#define WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT 1
static unsigned allocated, peak, chunks, payload, locks, fail_at, planned;
static struct { void *data; size_t bytes; } regions[2];
static bool exhausted, connected = true;
static unsigned char received[4096];
static void *reserve(size_t n, size_t size) {
    if (exhausted) return NULL;
    for (unsigned i = 0; i < 2; ++i) if (!regions[i].data) {
        void *data = calloc(n, size); assert(data);
        regions[i].data = data; regions[i].bytes = n * size;
        allocated += n * size; if (n * size > peak) peak = n * size;
        return data;
    }
    assert(false); return NULL;
}
static void release(void *data) {
    if (!data) return;
    for (unsigned i = 0; i < 2; ++i) if (regions[i].data == data) {
        allocated -= regions[i].bytes; regions[i].data = NULL; free(data); return;
    }
    assert(false);
}
#define calloc reserve
#define free release

typedef int esp_err_t;
typedef unsigned TickType_t;
typedef int ws_transport_opcodes_t;
typedef struct { int network_timeout_ms; } configuration_t;
typedef struct {
    int last_error, esp_tls_error_code, esp_tls_flags;
} error_t, *esp_tls_error_handle_t;
typedef struct {
    void *transport;
    char *tx_buffer, *rx_buffer;
    int buffer_size, tx_lock, lock, status_bits;
    atomic_bool send_ready;
} client_t, *esp_websocket_client_handle_t;
static int xSemaphoreTakeRecursive(int lock, unsigned timeout) { (void)lock; (void)timeout; ++locks; return pdPASS; }
static void xSemaphoreGiveRecursive(int lock) { (void)lock; assert(locks); --locks; }
static unsigned xEventGroupGetBits(int bits) { return bits; }
static bool esp_websocket_client_is_connected(client_t *client) { assert(client); return connected; }
static esp_tls_error_handle_t esp_transport_get_error_handle(void *transport) { assert(transport); return NULL; }
static const char *esp_err_to_name(int code) { (void)code; return "mock"; }
static void esp_websocket_client_error(client_t *client, const char *format, ...) { assert(client && format); }
static void esp_websocket_client_abort_connection(client_t *client, int kind) { assert(client && kind == 1); }
static int esp_transport_ws_send_raw(void *transport, int opcode, char *data, int bytes, int timeout) {
    assert(transport && timeout == 100 && bytes >= 0);
    assert(payload + bytes <= sizeof received);
    bool owns = false;
    for (unsigned i = 0; i < 2; ++i) if (regions[i].data == data && (unsigned)bytes <= regions[i].bytes) owns = true;
    assert(owns && !!(opcode & 128) == (payload + bytes == planned));
    if (!chunks) assert((opcode & 127) == (planned ? 2 : 0)); else assert(!(opcode & 127));
    chunks++;
    if (fail_at == chunks) return -1;
    memcpy(received + payload, data, bytes); payload += bytes;
    return bytes;
}
/* ACTUAL_ALLOCATOR */
/* ACTUAL_RELEASE */
/* ACTUAL_SEND */
#undef calloc
#undef free
static void send_case(client_t *client, unsigned bytes) {
    unsigned char data[4096], original[4096];
    assert(bytes <= sizeof data);
    for (unsigned i = 0; i < bytes; ++i) data[i] = (unsigned char)(i * 17);
    memcpy(original, data, bytes);
    unsigned baseline = allocated; planned = bytes;
    peak = chunks = payload = 0;
    assert(esp_websocket_client_send_with_exact_opcode(client, bytes ? 130 : 128, bytes ? data : NULL, bytes, 100) == (int)bytes);
    assert(peak == (bytes ? (bytes < 1280 ? bytes : 1280) : 1));
    assert(chunks == (bytes ? (bytes + 1279) / 1280 : 1));
    assert(payload == bytes && !memcmp(received, data, bytes) && !memcmp(original, data, bytes));
    assert(!locks && allocated == baseline && !client->tx_buffer);
}
int main(void) {
    client_t client = {.transport = (void *)1, .buffer_size = 1280, .send_ready = true};
    const unsigned sizes[] = {0, 1, 69, 968, 1279, 1280, 1281, 4096};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; ++i) send_case(&client, sizes[i]);
    unsigned char data[4096] = {0};
    chunks = payload = 0; fail_at = 2; planned = sizeof data;
    assert(esp_websocket_client_send_with_exact_opcode(&client, 130, data, sizeof data, 100) == -1);
    assert(!allocated && !client.tx_buffer && !locks && chunks == 2);
    fail_at = 0; exhausted = true;
    assert(esp_websocket_client_send_with_exact_opcode(&client, 130, data, 968, 100) == -1);
    assert(!allocated && !locks); exhausted = false;
    client.send_ready = false;
    assert(esp_websocket_client_send_with_exact_opcode(&client, 130, data, 968, 100) == -1);
    assert(!allocated && !locks); client.send_ready = true;
    // The receive buffer retains the entire configured speech packet capacity.
    assert(esp_websocket_new_buf(&client, false, 0) == ESP_OK && allocated == 1280);
    send_case(&client, 69); send_case(&client, 968); send_case(&client, 4096);
    assert(allocated == 1280 && client.rx_buffer); // Simultaneous full-capacity RX is untouched.
    esp_websocket_free_buf(&client, false); assert(!allocated);
    send_case(&client, 968);
    puts("WebSocket TX: exact short allocation, original fragmentation/FIN/bytes, zero, allocation/write failures and concurrent unchanged RX passed");
}
