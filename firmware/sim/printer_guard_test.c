#include <assert.h>
#include <stdio.h>
#include "printer_guard.h"
/* SDK_RESET_ENUM */
/* PRODUCTION_RESET_CLASSIFIER */
static void reset_classification(void) {
    const esp_reset_reason_t manual[] = {ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_USB, ESP_RST_JTAG};
    const esp_reset_reason_t fault[] = {ESP_RST_UNKNOWN, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT,
        ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO,
        ESP_RST_EFUSE, ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP};
    for (unsigned i=0;i<sizeof manual/sizeof *manual;i++) {
        printer_guard_begin();printer_guard_boot(maintenance_reset(manual[i]));
        assert(printer_guard_background_allowed());
    }
    for (unsigned i=0;i<sizeof fault/sizeof *fault;i++) {
        printer_guard_begin();printer_guard_boot(maintenance_reset(fault[i]));
        assert(!printer_guard_background_allowed());
        printer_guard_released();printer_guard_boot(maintenance_reset(fault[i]));
        assert(printer_guard_background_allowed());
    }
}
int main(void) {
    reset_classification();
    // esptool app-only USB reset starts discovery, but a new radio esp_restart fault blocks it again.
    printer_guard_begin();printer_guard_boot(maintenance_reset(ESP_RST_USB));
    assert(printer_guard_background_allowed());
    printer_guard_begin();printer_guard_boot(maintenance_reset(ESP_RST_SW));
    assert(!printer_guard_background_allowed());
    printer_guard_reset();assert(printer_guard_background_allowed());
    puts("printer guard: SDK reset reasons preserve crash suppression and permit explicit USB maintenance");
}
