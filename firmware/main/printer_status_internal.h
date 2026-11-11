#pragma once
#include "peripage_ble.h"
// Called only after Bluetooth close and binding persistence have completed.
void printer_status_observe(const peripage_info_t *info, bool verified, const char *error);
