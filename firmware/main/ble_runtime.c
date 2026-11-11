#include "ble_runtime.h"

#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"

#define STOP_STACK 3072
#define STOP_WAIT_MS 8000
static atomic_int runtime_owner;
static void (*runtime_sync)(void);
static StackType_t *host_stack, *stop_stack;
static StaticTask_t host_storage, stop_storage;
static TaskHandle_t host_task, stop_task;
static StaticSemaphore_t host_done_storage, stop_done_storage;
static SemaphoreHandle_t host_done, stop_done;
static esp_err_t stop_result;
static bool stop_started, runtime_fault;
static unsigned host_free;

bool ble_runtime_owned(ble_runtime_owner_t owner) { return atomic_load(&runtime_owner) == (int)owner; }

static void runtime_host(void *unused) {
    (void)unused;
    nimble_port_run();
    xSemaphoreGive(host_done);
    vTaskSuspend(NULL); // Stop worker joins before deleting the static task/stack.
}

static void runtime_synced(void) { if (runtime_sync) runtime_sync(); }

static void release_stacks(void) {
    heap_caps_free(host_stack); host_stack = NULL;
    heap_caps_free(stop_stack); stop_stack = NULL;
}

static bool join_suspended(TaskHandle_t task) {
    // A semaphore signal may wake the owner on another core before suspend.
    // Never free a static stack while that final instruction is still running.
    for (unsigned tick = 0; tick < 2000; tick++) {
        if (eTaskGetState(task) == eSuspended) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

static void runtime_stop_worker(void *unused) {
    (void)unused;
    // IDF's stop waits internally without a deadline; this owned task contains it.
    stop_result = nimble_port_stop() == 0 ? ESP_OK : ESP_FAIL;
    if (stop_result == ESP_OK && xSemaphoreTake(host_done, pdMS_TO_TICKS(2000)) != pdTRUE)
        stop_result = ESP_ERR_TIMEOUT;
    if (stop_result == ESP_OK && !join_suspended(host_task)) stop_result = ESP_ERR_TIMEOUT;
    if (stop_result == ESP_OK) {
        host_free = uxTaskGetStackHighWaterMark(host_task);
        vTaskDelete(host_task); host_task = NULL;
        stop_result = nimble_port_deinit();
    }
    xSemaphoreGive(stop_done);
    vTaskSuspend(NULL);
}

static bool valid_start(ble_runtime_owner_t owner, ble_runtime_prepare_t prepare, void (*sync)(void)) {
    return owner != BLE_RUNTIME_NONE && prepare && sync;
}

esp_err_t ble_runtime_start(ble_runtime_owner_t owner, ble_runtime_prepare_t prepare, void (*sync)(void)) {
    int expected = BLE_RUNTIME_NONE;
    if (!valid_start(owner, prepare, sync)) return ESP_ERR_INVALID_ARG;
    if (!atomic_compare_exchange_strong(&runtime_owner, &expected, owner)) return ESP_ERR_INVALID_STATE;
    host_done = xSemaphoreCreateBinaryStatic(&host_done_storage);
    stop_done = xSemaphoreCreateBinaryStatic(&stop_done_storage);
    stop_started = runtime_fault = false; stop_result = ESP_OK; runtime_sync = sync; host_free = 0;
    // C6 LP SRAM is needed by the speaker ring; these stacks use DRAM only.
    unsigned stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
    host_stack = heap_caps_malloc(CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE, stack_caps);
    stop_stack = heap_caps_malloc(STOP_STACK, stack_caps);
    esp_err_t result = host_stack && stop_stack ? nimble_port_init() : ESP_ERR_NO_MEM;
    if (result == ESP_OK) {
        ble_hs_cfg.sync_cb = runtime_synced;
        result = prepare();
        if (result == ESP_OK) {
            host_task = xTaskCreateStaticPinnedToCore(runtime_host, "nimble_host", CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE,
                NULL, configMAX_PRIORITIES - 4, host_stack, &host_storage, CONFIG_BT_NIMBLE_PINNED_TO_CORE);
            result = host_task ? ESP_OK : ESP_ERR_NO_MEM;
        }
        if (result != ESP_OK && nimble_port_deinit() != ESP_OK) runtime_fault = true;
    }
    if (result != ESP_OK) {
        runtime_sync = NULL; release_stacks();
        if (!runtime_fault) atomic_store(&runtime_owner, BLE_RUNTIME_NONE);
    }
    return result;
}

esp_err_t ble_runtime_stop(ble_runtime_owner_t owner) {
    if (!ble_runtime_owned(owner) || owner == BLE_RUNTIME_NONE) return ESP_ERR_INVALID_STATE;
    if (runtime_fault) return ESP_FAIL;
    if (!stop_started) {
        stop_started = true;
        stop_task = xTaskCreateStaticPinnedToCore(runtime_stop_worker, "ble_stop", STOP_STACK, NULL, 4,
            stop_stack, &stop_storage, CONFIG_BT_NIMBLE_PINNED_TO_CORE);
        if (!stop_task) return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(stop_done, pdMS_TO_TICKS(STOP_WAIT_MS)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!join_suspended(stop_task)) { runtime_fault = true; return ESP_ERR_TIMEOUT; }
    unsigned stop_free = uxTaskGetStackHighWaterMark(stop_task);
    vTaskDelete(stop_task); stop_task = NULL;
    if (stop_result != ESP_OK) { runtime_fault = true; return stop_result; }
    runtime_sync = NULL; release_stacks(); atomic_store(&runtime_owner, BLE_RUNTIME_NONE);
    ESP_LOGI("ble_runtime", "released host-stack-free=%u stop-stack-free=%u", host_free, stop_free);
    return ESP_OK;
}
