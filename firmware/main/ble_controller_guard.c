#include "sdkconfig.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t __real_esp_bt_controller_enable(esp_bt_mode_t mode);

// A conservative admission check, not an exact ROM allocation budget or heap
// reservation. SDK initialization, validation and failure cleanup stay intact.
esp_err_t __wrap_esp_bt_controller_enable(esp_bt_mode_t mode) {
    if (mode != ESP_BT_MODE_BLE || esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_INITED)
        return __real_esp_bt_controller_enable(mode);
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t stack = CONFIG_BT_LE_CONTROLLER_TASK_STACK_SIZE;
    // Policy reserve: 4096 bytes for JSON/work, plus 512 bytes of allocator margin.
    const size_t required = stack + sizeof(StaticTask_t) + 4096u + 512u;
    size_t largest = heap_caps_get_largest_free_block(caps);
    size_t available = heap_caps_get_free_size(caps);
    if (largest < stack || available < required) {
        ESP_LOGW("ble_guard", "controller refused: free=%u largest=%u need=%u stack=%u",
            (unsigned)available, (unsigned)largest, (unsigned)required, (unsigned)stack);
        return ESP_ERR_NO_MEM;
    }
    return __real_esp_bt_controller_enable(mode);
}
