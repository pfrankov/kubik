// mbedTLS dynamic records share RAM with continuous microphone and playback.
// Reserve one 1 KB TX scratch slot and one full RX record before audio starts;
// other TLS allocations use the internal heap.
#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/platform.h"
#include "tls_mem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SLOT_BYTES 1536  // OUT=1024 plus TLS header, MAC and padding (1365 measured)
#define SLOT_MIN 512     // small microphone TX also stays out of the full RX reservation

// IDF allocates a full TX scratch buffer even for the WS header: 1365 bytes
// with OUT=1024. Speech RX is 1574 bytes for a 100 ms IMA packet. Keep TX
// in the fixed slot and allow RX to use the full-record reservation.
#define SPEECH_MIN 1536
#define RECORD_TRIGGER_BYTES 4864  // only a large idle RX record pre-reserves full capacity
_Static_assert(CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN <= 1024, "Live requires a 1 KB TLS TX buffer");

static uint8_t s_slot[SLOT_BYTES] __attribute__((aligned(16)));
static atomic_bool s_slot_busy;
// TLS may coalesce a burst into a full 16 KB record. Reserve it before the audio
// ring fragments the heap, after the wake model has released its arena.
// Include TLS framing and allocator metadata, retaining the full 16 KB RX limit.
// This same workspace holds the Classic/Realtime capture queue between phases.
#define SPEECH_RECORD_BYTES (CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN + 1024)
static SemaphoreHandle_t s_lock;
static void *s_record;
static bool s_record_busy, s_speech, s_turn, s_capture;

static void release_record_locked(void) {
    if (!s_speech && !s_turn && !s_record_busy && !s_capture) { heap_caps_free(s_record); s_record = NULL; }
}

static void reserve_locked(bool active) {
    if (active && !s_record)
        s_record = heap_caps_malloc(SPEECH_RECORD_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (active && !s_record) ESP_LOGW("tls_mem", "speech record reservation failed");
    release_record_locked();
}

void tls_mem_speech(bool active) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_speech = active; reserve_locked(active);
    xSemaphoreGive(s_lock);
}

bool tls_mem_turn(bool active) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_turn = active; reserve_locked(active);
    bool ready = !active || s_record;
    if (!ready) s_turn = false;
    xSemaphoreGive(s_lock);
    return ready;
}

void *tls_mem_capture_take(size_t bytes) {
    if (bytes > SPEECH_RECORD_BYTES) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    void *record = NULL;
    if (!s_capture && !s_record_busy && !s_speech) {
        if (!s_record) s_record = heap_caps_malloc(SPEECH_RECORD_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        record = s_record;
        s_capture = record != NULL;
    }
    xSemaphoreGive(s_lock);
    return record;
}

void tls_mem_capture_release(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_capture = false;
    release_record_locked();
    xSemaphoreGive(s_lock);
}

static void *speech_record(size_t bytes) {
    // Keep the full RX reservation available for a coalesced incoming record.
    // A simultaneous small microphone TX must not borrow it and force RX to
    // allocate another buffer. Normal speech RX also uses this reservation;
    // keeping it idle while RX uses the heap defeats its purpose.
    if (bytes < SPEECH_MIN || bytes > SPEECH_RECORD_BYTES) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // A coalesced record may itself contain `speak`. Allocate its full capacity
    // now so speech can retain this very buffer instead of allocating a second one.
    if (!s_record && bytes > RECORD_TRIGGER_BYTES)
        s_record = heap_caps_malloc(SPEECH_RECORD_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void *record = s_record && !s_record_busy && !s_capture ? s_record : NULL;
    if (record) s_record_busy = true;
    xSemaphoreGive(s_lock);
    if (record) memset(record, 0, bytes);
    return record;
}

static void *tls_calloc(size_t n, size_t size) {
    size_t bytes = n * size;
    if (size && bytes / size != n) return NULL;
    void *record = speech_record(bytes);
    if (record) return record;
    if (bytes >= SLOT_MIN && bytes <= SLOT_BYTES && !atomic_exchange(&s_slot_busy, true)) {
        memset(s_slot, 0, bytes);
        return s_slot;
    }
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#ifdef KUBIK_HEAP_PROBE
    if (bytes >= 384) ESP_LOGI("tls_mem", "alloc %u -> %p free=%u largest=%u", (unsigned)bytes, p,
                               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#endif
    if (!p) ESP_LOGW("tls_mem", "alloc %u failed, largest %u", (unsigned)bytes,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return p;
}

static void tls_free(void *p) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool reserved = p && p == s_record;
    if (reserved) { s_record_busy = false; release_record_locked(); }
    xSemaphoreGive(s_lock);
    if (reserved) return;
    if (p == s_slot) atomic_store(&s_slot_busy, false);
    else {
#ifdef KUBIK_HEAP_PROBE
        size_t sz = p ? heap_caps_get_allocated_size(p) : 0;
        heap_caps_free(p);
        if (sz >= 384) ESP_LOGI("tls_mem", "free %u %p free=%u", (unsigned)sz, p, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
        heap_caps_free(p);
#endif
    }
}

void tls_mem_init(void) {
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_lock ? ESP_OK : ESP_ERR_NO_MEM);
    mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
}
