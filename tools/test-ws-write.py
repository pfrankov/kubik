#!/usr/bin/env python3
"""Run the actual pinned SDK framing through our bounded TLS write coalescer."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
idf = Path(os.environ.get('IDF_PATH', str(Path.home() / 'esp/esp-idf-v5.5.1')))
sdk = (idf / 'components/tcp_transport/transport_ws.c').read_text()
framing = sdk[sdk.index('static int _ws_write('):sdk.index('static int ws_write(')]
framing = framing.replace('int esp_transport_ws_send_raw(', 'int __real_esp_transport_ws_send_raw(')
source = '\n'.join(line for line in (ROOT / 'firmware/main/link_tcp.c').read_text().splitlines()
                   if not line.startswith('#include'))
mock = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
typedef void *esp_transport_handle_t;
typedef void *TaskHandle_t;
typedef int ws_transport_opcodes_t;
#define MAX_WEBSOCKET_HEADER_SIZE 14
#define WS_MASK 128
#define WS_SIZE16 126
#define WS_SIZE64 127
#define ESP_ERR_INVALID_ARG -1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define IPPROTO_TCP 6
#define TCP_NODELAY 1
static TaskHandle_t task = (void *)1;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return task; }
static bool socket_error;
static int setsockopt(int socket, int level, int option, const void *value, unsigned bytes) {
    assert(socket == 7 && level == IPPROTO_TCP && option == TCP_NODELAY && bytes == sizeof(int));
    assert(*(const int *)value == 1); return socket_error ? -1 : 0;
}
static int esp_transport_get_socket(void *t) { assert(t); return 7; }
typedef struct { esp_transport_handle_t parent; } transport_ws_t;
static transport_ws_t ctx = { (void *)2 };
static void *esp_transport_get_context_data(void *t) { assert(t == (void *)1); return &ctx; }
static uint8_t ws_get_bin_opcode(ws_transport_opcodes_t opcode) { return opcode; }
static int poll = 1;
static int esp_transport_poll_write(void *t, int timeout) { assert(t == ctx.parent && timeout == 100); return poll; }
static ssize_t getrandom(void *b, size_t n, int flags) {
    assert(n == 4 && flags == 0); memcpy(b, "abcd", n); return n;
}
static unsigned char wire[4096];
static int writes, length, partial;
int __real_esp_transport_write(void *t, const char *b, int n, int timeout) {
    assert(t == ctx.parent && timeout == 100 && n > 0 && n <= 1024);
    writes++; memcpy(wire + length, b, n); length += n;
    return partial ? n - 1 : n;
}
#define esp_transport_write __wrap_esp_transport_write
'''
tests = r'''
static void frame(int bytes, int opcode) {
    char data[2048], original[2048];
    for (int i = 0; i < bytes; i++) data[i] = i % 251;
    memcpy(original, data, bytes); writes = length = 0;
    assert(__wrap_esp_transport_ws_send_raw((void *)1, opcode, data, bytes, 100) == bytes);
    int header = bytes <= 125 ? 6 : 8;
    assert(wire[0] == opcode && length == header + bytes);
    assert((wire[1] & 128) && (wire[1] & 127) == (bytes <= 125 ? bytes : 126));
    if (header == 8) assert(((int)wire[2] << 8 | wire[3]) == bytes);
    for (int i = 0; i < bytes; i++) assert((wire[header + i] ^ "abcd"[i % 4]) == (unsigned char)original[i]);
    assert(!memcmp(data, original, bytes));
    assert(writes == (header + bytes + 1023) / 1024 && !atomic_load(&s_owner));
}
int main(void) {
    for (int opcode = 0x81; opcode <= 0x82; opcode++) {
        frame(0, opcode); frame(125, opcode); frame(126, opcode); frame(968, opcode); frame(2048, opcode);
    }
    char data[968], copy[968]; memset(data, 42, sizeof data); memcpy(copy, data, sizeof data);
    writes = length = 0; partial = 1;
    assert(__wrap_esp_transport_ws_send_raw((void *)1, 0x82, data, 968, 100) == -1);
    assert(!memcmp(data, copy, 968) && !atomic_load(&s_owner)); partial = 0; frame(968, 0x82);
    poll = 0; assert(__wrap_esp_transport_ws_send_raw((void *)1, 0x89, NULL, 0, 100) == 0);
    assert(!atomic_load(&s_owner)); poll = 1;
    socket_error = true; assert(__wrap_esp_transport_ws_send_raw((void *)1, 0x82, data, 968, 100) == -1);
    socket_error = false; frame(0, 0x89); frame(0, 0x8a); frame(0, 0x88);
    atomic_store(&s_owner, (void *)1); task = (void *)3; writes = length = 0;
    assert(__wrap_esp_transport_write(ctx.parent, "foreign", 7, 100) == 7 && length == 7);
    atomic_store(&s_owner, NULL);
    puts("WS: pinned SDK framing, one Live TLS record, bounded larger writes, masking, failures, control and foreign tasks passed");
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-ws-write-') as temp:
    src, exe = Path(temp) / 'test.c', Path(temp) / 'test'
    src.write_text(mock + source + framing + tests)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g', '-O1', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
