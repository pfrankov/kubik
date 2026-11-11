#include "printer_platform.h"
#include "printer_guard.h"
#include "app_wake.h"
#include "task_placement.h"

#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Actual C6 status/text/QR left only 276 bytes free with the former 3072-byte stack.
#define WORK_STACK 4096
static StackType_t *stack;
static StaticTask_t task_storage;
static TaskHandle_t task;
static void (*work)(void *);
static void *work_context;
static atomic_bool finished;
static bool booted;

static bool maintenance_reset(esp_reset_reason_t reason) {
    return reason == ESP_RST_POWERON || reason == ESP_RST_EXT ||
        reason == ESP_RST_USB || reason == ESP_RST_JTAG;
}

void printer_platform_boot(void) {
    if (!booted) {
        printer_guard_boot(maintenance_reset(esp_reset_reason()));
        booted = true;
    }
}
int64_t printer_platform_now_ms(void) { return esp_timer_get_time() / 1000; }
int printer_platform_reset_reason(void) { return esp_reset_reason(); }

static void run(void *unused) {
    (void)unused;
    work(work_context);
    ESP_LOGI("printer", "worker stack-free=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    atomic_store_explicit(&finished, true, memory_order_release);
    vTaskSuspend(NULL);
}

bool printer_platform_launch(void (*callback)(void *), void *context) {
    if (task || !callback) return false;
    app_wake_configuration_busy(true);
    stack = heap_caps_malloc(WORK_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    work = callback; work_context = context;
    atomic_store(&finished, false);
    if (stack) task = xTaskCreateStaticPinnedToCore(run, "printer", WORK_STACK, NULL, 3,
        stack, &task_storage, KUBIK_UI_CORE);
    if (task) return true;
    heap_caps_free(stack); stack = NULL;
    app_wake_configuration_busy(false);
    return false;
}

bool printer_platform_finished(void) {
    return task && atomic_load_explicit(&finished, memory_order_acquire) && eTaskGetState(task) == eSuspended;
}

void printer_platform_reap(void) {
    if (!printer_platform_finished()) return;
    vTaskDelete(task); task = NULL;
    heap_caps_free(stack); stack = NULL;
    work = NULL; work_context = NULL;
}

void printer_platform_release(void) { app_wake_configuration_busy(false); }

void printer_platform_wait_tick(void) { vTaskDelay(pdMS_TO_TICKS(1)); }

void printer_platform_radio_fault(void) {
    ESP_LOGE("printer", "BLE shutdown failed; restarting with automatic discovery suppressed");
    // The RTC inflight marker remains set. No response may release a host lease
    // while this controller still owns voice memory.
    esp_restart();
}
