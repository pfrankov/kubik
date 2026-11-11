#include "printer_guard.h"
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define RTC_NOINIT_ATTR
#endif

#define INFLIGHT_MAGIC UINT32_C(0x50524e32)
static RTC_NOINIT_ATTR uint32_t inflight, inverse;
static bool background_blocked;

void printer_guard_boot(bool maintenance_reset) {
    if (maintenance_reset) { inflight = inverse = 0; }
    background_blocked = inflight == INFLIGHT_MAGIC && inverse == ~INFLIGHT_MAGIC;
}
bool printer_guard_background_allowed(void) { return !background_blocked; }
void printer_guard_begin(void) { inverse = ~INFLIGHT_MAGIC; inflight = INFLIGHT_MAGIC; }
void printer_guard_released(void) { inflight = inverse = 0; }
void printer_guard_reset(void) { printer_guard_released(); background_blocked = false; }
