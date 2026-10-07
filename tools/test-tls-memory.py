#!/usr/bin/env python3
"""Exercise the actual TLS allocator: record reservation, pressure, stop and reuse."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
body = '\n'.join(line for line in (ROOT / 'firmware/main/tls_mem.c').read_text().splitlines()
                 if not line.startswith('#include'))
mock = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 1024
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define ESP_OK 0
#define ESP_ERR_NO_MEM -1
#define ESP_ERROR_CHECK(x) assert((x) == 0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define portMAX_DELAY 0
typedef int SemaphoreHandle_t;
static bool held, pressure;
static unsigned frees;
static int xSemaphoreCreateMutex(void) { return 1; }
static void xSemaphoreTake(int m, int t) { (void)m; (void)t; assert(!held); held = true; }
static void xSemaphoreGive(int m) { (void)m; assert(held); held = false; }
static void *heap_caps_malloc(size_t n, int flags) { (void)flags; return pressure ? NULL : malloc(n); }
static void *heap_caps_calloc(size_t n, size_t size, int flags) { (void)flags; return pressure ? NULL : calloc(n, size); }
static void heap_caps_free(void *p) { if (p) { frees++; } free(p); }
static void mbedtls_platform_set_calloc_free(void *(*a)(size_t,size_t), void (*f)(void *)) { assert(a && f); }
'''
tests = r'''
int main(void) {
    tls_mem_init();
    void *small = tls_calloc(1, 1365); assert(small == s_slot);
    void *incoming = tls_calloc(1, 14903); assert(incoming == s_record);
    tls_mem_speech(true); assert(s_record == incoming && s_record_busy); // adopt the record containing speak
    tls_free(incoming); assert(s_record && !s_record_busy);
    pressure = true;
    void *large = tls_calloc(1, 16600); assert(large == s_record && s_record_busy);
    assert(tls_calloc(1, 15000) == NULL); // one in-flight full record, never shared
    assert(tls_calloc(SIZE_MAX, 2) == NULL); // multiplication overflow
    memset(large, 7, 16600);
    tls_mem_speech(false); assert(s_record && frees == 0); // end waits for the reader
    tls_free(large); assert(!s_record && frees == 1);
    tls_free(small); assert(!s_slot_busy);
    pressure = false; tls_mem_speech(true);
    large = tls_calloc(1, 16600); assert(large == s_record);
    for (int i = 0; i < 16600; i++) assert(((char *)large)[i] == 0);
    tls_free(large); assert(s_record && !s_record_busy);
    pressure = true;
    void *frame = tls_calloc(1, 1365); assert(frame == s_slot && !s_record_busy);
    large = tls_calloc(1,1574); assert(large==s_record); // Normal speech RX uses the reserved buffer too.
    tls_free(large);
    large = tls_calloc(1,16600); assert(large==s_record); // Duplex TX cannot displace a full RX record.
    tls_free(large);
    large = tls_calloc(1,17117); assert(large==s_record); // Reserve capacity boundary; no enabled static-RX mode is assumed.
    tls_free(large);
    tls_free(frame); pressure = false;
    tls_mem_speech(false); assert(!s_record && frees == 2);
    void *context = tls_calloc(3, 100); assert(context && context != s_slot);
    tls_free(context); assert(frees == 3);
    tls_mem_turn(true); assert(s_record);
    pressure = true; tls_mem_speech(true); // VAD fragmented the heap after turn admission.
    void *turn_record = tls_calloc(1, 14903); assert(turn_record == s_record);
    tls_mem_turn(false); assert(s_record); // playback independently retains the lease.
    tls_mem_speech(false); assert(s_record); // RX still owns it.
    tls_free(turn_record); assert(!s_record);
    pressure = false; tls_mem_turn(true); tls_mem_turn(false); assert(!s_record); // cancelled/empty turn
    pressure = true; assert(!tls_mem_turn(true) && !s_turn && !s_record);
    assert(tls_mem_turn(false));
    pressure=false; assert(tls_mem_turn(true));
    void *capture=tls_mem_capture_take(15392); assert(capture==s_record && s_capture);
    memset(capture,0x5a,15392);
    void *tx=tls_calloc(1,800); assert(tx && tx!=capture); // No TLS alias into queued PCM.
    for(int i=0;i<15392;i++)assert(((unsigned char *)capture)[i]==0x5a);
    assert(!tls_mem_capture_take(15392)); // exclusive lease
    tls_mem_turn(false); assert(s_record==capture); // disconnect waits for the worker
    tls_free(tx); tls_mem_capture_release(); assert(!s_record);
    assert(tls_mem_turn(true));
    void *rx=tls_calloc(1,16000); assert(rx==s_record);
    assert(!tls_mem_capture_take(15392)); // an in-flight TLS record cannot become PCM
    tls_free(rx); capture=tls_mem_capture_take(15392); assert(capture==s_record);
    tls_mem_capture_release(); rx=tls_calloc(1,16000); assert(rx==capture);
    tls_free(rx); tls_mem_turn(false); assert(!s_record);
    pressure=true; assert(!tls_mem_capture_take(15392)); assert(!s_capture && !s_record);
    pressure=false;
    for(size_t size=1537;size<=4400;size+=2863) {
        void *idle=tls_calloc(1,size); assert(idle && idle!=s_slot && !s_record);
        tls_free(idle); assert(!s_record);
    }
    void *idle_record=tls_calloc(1,4865); assert(idle_record==s_record && s_record_busy);
    tls_free(idle_record); assert(!s_record);
    puts("TLS: full record under heap pressure; delayed release, reuse and overflow checks passed");
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-tls-') as temp:
    source, binary = Path(temp) / 'test.c', Path(temp) / 'test'
    source.write_text(mock + body + tests)
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
