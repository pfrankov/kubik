#include "character.h"
#include "frame_diagnostics.h"
#include <stdio.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "status_json.h"
// The display publishes one compact window; USB formats it after releasing the
// critical section. No allocation or wait is added to rendering or playback.
static frame_diagnostics_snapshot_t s_snapshot;
static atomic_uint s_requested, s_next;
static SemaphoreHandle_t s_ready, s_reader;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_sequence;
void frame_diagnostics_init(void) {
    s_ready = xSemaphoreCreateBinary();
    s_reader = xSemaphoreCreateMutex();
    configASSERT(s_ready && s_reader);
}
bool frame_diagnostics_requested(void) { return atomic_load(&s_requested); }
void frame_diagnostics_publish(const frame_diagnostics_snapshot_t *snapshot) {
    uint32_t request = atomic_exchange(&s_requested, 0);
    if (!request) return;
    portENTER_CRITICAL(&s_mux);
    s_snapshot = *snapshot;
    s_sequence = request;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_ready);
}
void frame_diagnostics_reply(char *reply, size_t capacity) {
    if (xSemaphoreTake(s_reader, pdMS_TO_TICKS(50)) != pdTRUE) {
        snprintf(reply, capacity, "{\"ok\":false,\"error\":\"frame diagnostics busy\"}"); return;
    }
    xSemaphoreTake(s_ready, 0);
    uint32_t request = atomic_fetch_add(&s_next, 1) + 1;
    atomic_store(&s_requested, request);
    TickType_t started = xTaskGetTickCount(), remaining = pdMS_TO_TICKS(500);
    bool ready = false;
    while (remaining && xSemaphoreTake(s_ready, remaining) == pdTRUE) {
        portENTER_CRITICAL(&s_mux);
        ready = s_sequence == request;
        portEXIT_CRITICAL(&s_mux);
        if (ready) break;
        TickType_t elapsed = xTaskGetTickCount() - started;
        remaining = elapsed >= pdMS_TO_TICKS(500) ? 0 : pdMS_TO_TICKS(500) - elapsed;
    }
    if (!ready) { unsigned pending = request; atomic_compare_exchange_strong(&s_requested, &pending, 0); }
    status_json_t json;
    status_json_begin(&json, reply, capacity);
    status_json_bool(&json, "ok", ready);
    if (ready) {
        frame_diagnostics_snapshot_t snapshot;
        portENTER_CRITICAL(&s_mux);
        snapshot = s_snapshot;
        portEXIT_CRITICAL(&s_mux);
        status_json_integer(&json, "sequence", request);
        status_json_integer(&json, "frames", snapshot.frames);
        status_json_integer(&json, "elapsed_us", snapshot.elapsed_us);
        status_json_integer(&json, "mean_us", snapshot.mean_us);
        status_json_integer(&json, "max_us", snapshot.max_us);
        status_json_integer(&json, "late", snapshot.late);
        status_json_integer(&json, "px", snapshot.px);
        status_json_integer(&json, "prims", snapshot.prims);
        status_json_integer(&json, "face", snapshot.face);
        status_json_integer(&json, "shapes", snapshot.shapes);
        status_json_integer(&json, "prep", snapshot.prep);
        status_json_integer(&json, "image", snapshot.image);
        status_json_integer(&json, "glass", snapshot.glass);
        status_json_integer(&json, "hash", snapshot.hash);
        status_json_integer(&json, "send", snapshot.send);
        status_json_integer(&json, "gap_us", snapshot.gap_us);
        status_json_integer(&json, "modes", snapshot.modes);
        status_json_integer(&json, "presented", snapshot.presented);
        status_json_integer(&json, "overflows", snapshot.overflows);
        status_json_integer(&json, "active", snapshot.active);
        status_json_integer(&json, "slow_frames", snapshot.slow_frames);
        status_json_integer(&json, "deadline_misses", snapshot.deadline_misses);
        status_json_string(&json, "character", character_name(KUBIK_CHARACTER));
    } else status_json_string(&json, "error", "frame diagnostics timed out");
    if (!status_json_end(&json)) snprintf(reply, capacity, "{\"ok\":false}");
    xSemaphoreGive(s_reader);
}
