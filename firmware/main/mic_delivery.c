#include "mic_delivery.h"

#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "tls_mem.h"
#include "ima_adpcm.h"

#define FRAMES 30
#define SAMPLES 960
#define LIVE_FRAMES 16
#define IMA_BYTES (IMA_HEADER_BYTES + SAMPLES / 2)
#define STACK_BYTES 4096
typedef struct { uint32_t epoch; uint8_t ima[IMA_HEADER_BYTES + SAMPLES / 2]; } frame_t;
#define WORKSPACE_BYTES (FRAMES * sizeof(frame_t) + SAMPLES * sizeof(int16_t))
static frame_t *s_frames;
static int16_t *s_pcm;
static ima_state_t s_encoder;
static bool s_duplex;
static size_t s_capacity, s_stride, s_workspace_bytes;
static frame_t *queued_frame(unsigned index) {
    return (frame_t *)((uint8_t *)s_frames + (index % s_capacity) * s_stride);
}
// Only the active sender stack leases LP SRAM. The packed queue and its decode
// scratch share the existing TLS workspace and release it before playback.
static StackType_t *s_stack;
static StaticTask_t s_task_storage;
static TaskHandle_t s_task;
static StaticSemaphore_t s_done_storage;
static SemaphoreHandle_t s_done;
static atomic_uint s_read, s_write;
static atomic_bool s_stopping, s_failed;
static uint32_t s_failed_epoch, s_peak;
static mic_delivery_frame_fn s_deliver;
static void *s_context;

static void delivery_task(void *unused) {
    (void)unused;
    bool reported = false;
    for (;;) {
        unsigned read = atomic_load(&s_read), write = atomic_load(&s_write);
        if (atomic_load(&s_failed)) {
            if (!reported) { s_deliver(s_context, NULL, NULL, s_failed_epoch); reported = true; }
            atomic_store(&s_read, write);
        } else if (read != write) {
            frame_t *frame = queued_frame(read);
            if (!s_duplex) {
                ima_state_t decoder; ima_read_header(frame->ima, &decoder);
                ima_decode(&decoder, frame->ima + IMA_HEADER_BYTES, SAMPLES / 2, s_pcm);
            }
            s_deliver(s_context, s_pcm, frame->ima, frame->epoch);
            atomic_store(&s_read, read + 1);
            continue;
        }
        if (atomic_load(&s_stopping)) {
            if (atomic_load(&s_read) == atomic_load(&s_write)) break;
            continue;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    xSemaphoreGive(s_done);
    vTaskSuspend(NULL); // Owner deletes and frees the static stack after the join.
}

static void release_buffers(void) {
    // Called only after the worker joined, or before it could start.
    if (s_frames) {
        memset(s_frames, 0, s_workspace_bytes);
        if (s_duplex) heap_caps_free(s_frames);
        else tls_mem_capture_release();
    }
    s_frames = NULL; s_pcm = NULL;
    heap_caps_free(s_stack); s_stack = NULL;
}

static bool reserve_queue(bool duplex) {
    s_duplex = duplex;
    s_capacity = duplex ? LIVE_FRAMES : FRAMES;
    s_stride = duplex ? sizeof(frame_t) + IMA_BYTES + 1 : sizeof(frame_t); // 4-byte aligned
    s_workspace_bytes = s_capacity * s_stride + (duplex ? 0 : SAMPLES * sizeof(int16_t));
    if (duplex) s_frames = heap_caps_malloc(s_workspace_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Classic/Realtime lease TLS memory; Live must receive TLS while still capturing.
    // A TLS reader may briefly own the workspace after the PTT control frame.
    for (int retry = 0; retry < 10 && !s_frames && !duplex; retry++) {
        s_frames = tls_mem_capture_take(WORKSPACE_BYTES);
        if (!s_frames) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!s_frames) return false;
    s_pcm = duplex ? NULL : (int16_t *)((uint8_t *)s_frames + s_capacity * s_stride);
    return true;
}

bool mic_delivery_start(mic_delivery_frame_fn deliver, void *context, bool duplex) {
    if (s_task) return false;
    if (!reserve_queue(duplex)) return false;
    s_encoder = (ima_state_t){0};
    s_stack = heap_caps_malloc(duplex ? 3072 : STACK_BYTES, MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT);
    if (!s_stack) { release_buffers(); return false; }
    atomic_store(&s_read, 0); atomic_store(&s_write, 0);
    atomic_store(&s_stopping, false); atomic_store(&s_failed, false);
    s_deliver = deliver; s_context = context; s_peak = 0;
    s_done = xSemaphoreCreateBinaryStatic(&s_done_storage);
    s_task = xTaskCreateStatic(delivery_task, "mic_send", duplex ? 3072 : STACK_BYTES, NULL, duplex ? 6 : 7, s_stack, &s_task_storage);
    if (!s_task) { release_buffers(); return false; }
    return true;
}

void mic_delivery_push(const int16_t *pcm, uint32_t epoch, const uint8_t *reference) {
    if (!s_task || atomic_load(&s_stopping) || atomic_load(&s_failed)) return;
    unsigned write = atomic_load(&s_write), read = atomic_load(&s_read);
    if (!pcm || (s_duplex && !reference) || write - read == s_capacity) {
        s_failed_epoch = epoch;
        atomic_store(&s_failed, true);
    } else {
        frame_t *frame = queued_frame(write);
        frame->epoch = epoch;
        ima_encode(&s_encoder, pcm, SAMPLES, frame->ima);
        if (s_duplex) memcpy(frame->ima + IMA_BYTES, reference, IMA_BYTES);
        atomic_store(&s_write, write + 1);
        if (write + 1 - read > s_peak) s_peak = write + 1 - read;
    }
    xTaskNotifyGive(s_task);
}

void mic_delivery_stop(void) {
    if (!s_task) return;
    atomic_store(&s_stopping, true);
    xTaskNotifyGive(s_task);
    xSemaphoreTake(s_done, portMAX_DELAY); // TLS itself has a bounded send timeout.
    ESP_LOGI("audio", "mic delivery peak=%ums stack-free=%u", (unsigned)(s_peak * 40),
             (unsigned)uxTaskGetStackHighWaterMark(s_task));
    vTaskDelete(s_task); s_task = NULL;
    release_buffers();
}
