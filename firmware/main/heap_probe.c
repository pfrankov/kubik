#include "heap_probe.h"

#include <stdio.h>
#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_rom_sys.h"

static atomic_uint s_failures;
unsigned hp_allocation_failures(void) { return atomic_load(&s_failures); }

static void allocation_failed(size_t bytes, uint32_t caps, const char *function) {
    unsigned failures = atomic_fetch_add(&s_failures, 1) + 1;
    if (failures <= 16) esp_rom_printf("hp: allocation failed bytes=%u caps=%x free=%u largest=%u in %s\n",
        (unsigned)bytes, (unsigned)caps, (unsigned)heap_caps_get_free_size(caps),
        (unsigned)heap_caps_get_largest_free_block(caps), function);
}

#define HP_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

// KUBIK_HEAP_PROBE (cmake -DKUBIK_HEAP_PROBE=1, debug builds only): also the lowest free size since the previous
// checkpoint, which shows the peak of a TLS handshake that the boot-wide minimum hides, and on demand the heap map.
#ifdef KUBIK_HEAP_PROBE
// Stack headroom (the least ever free, bytes) of the tasks that matter.
static void hp_stacks(void) {
    static const char *const names[] = {"main", "display", "app", "power", "spk", "mic", "link", "input", "usb_rx",
                                        "websocket_task", "tiT", "sys_evt", "esp_timer", "wifi", "Tmr Svc", "ipc0", "IDLE", "httpd", "dns"};
    char line[300] = "";
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        TaskHandle_t t = xTaskGetHandle(names[i]);
        if (t) snprintf(line + strlen(line), sizeof line - strlen(line), " %s=%u", names[i], (unsigned)uxTaskGetStackHighWaterMark(t));
    }
    ESP_LOGI("hp", "stack free:%s", line);
}
#endif

void hp_mark(const char *phase) {
    static bool registered;
    if (!registered) { heap_caps_register_failed_alloc_callback(allocation_failed); registered = true; }
#ifdef KUBIK_HEAP_PROBE
    size_t local = heap_caps_get_minimum_free_size(HP_CAPS);
    size_t free = heap_caps_get_free_size(HP_CAPS), largest = heap_caps_get_largest_free_block(HP_CAPS);
    heap_caps_monitor_local_minimum_free_size_stop();  // the boot-wide minimum takes the lower of both
    size_t boot_min = heap_caps_get_minimum_free_size(HP_CAPS);
    heap_caps_monitor_local_minimum_free_size_start();
    ESP_LOGI("hp", "%s free=%u largest=%u min=%u since-last-min=%u", phase, (unsigned)free, (unsigned)largest,
             (unsigned)boot_min, (unsigned)local);
    if (!strncmp(phase, "ws connected", 12) || !strcmp(phase, "speech played")) hp_stacks();
#else
    ESP_LOGI("hp", "%s free=%u largest=%u min=%u", phase, (unsigned)heap_caps_get_free_size(HP_CAPS),
             (unsigned)heap_caps_get_largest_free_block(HP_CAPS), (unsigned)heap_caps_get_minimum_free_size(HP_CAPS));
#endif
    ESP_LOGI("hp", "pools dma=%u/%u lp=%u/%u free/largest",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT));
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        ESP_LOGI("hp", "wifi rssi=%d channel=%u", ap.rssi, ap.primary);
}
