#pragma once

#include "peripage_ble.h"
#include "host/ble_hs.h"

void peripage_gatt_begin(uint32_t epoch, uint16_t connection);
void peripage_gatt_init(void);
peripage_result_t peripage_gatt_setup(peripage_info_t *info, int64_t deadline_us);
bool peripage_gatt_write(const uint8_t *data, size_t size, int64_t deadline_us);
bool peripage_gatt_finish(int64_t deadline_us);
void peripage_gatt_notify(uint16_t handle, const struct os_mbuf *data);
void peripage_gatt_lost(void);
// Called by immutable callback epochs, never a pointer reused by a new open.
bool peripage_link_current(uint32_t epoch);
