#pragma once
#include <stdbool.h>

// Power-on or explicit external/USB/JTAG reset starts a new maintenance run.
// Software/crash/watchdog resets preserve the inflight loop guard.
void printer_guard_boot(bool maintenance_reset);
bool printer_guard_background_allowed(void);
void printer_guard_begin(void);
void printer_guard_released(void);
void printer_guard_reset(void);
