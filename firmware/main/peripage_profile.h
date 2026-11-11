#pragma once

#include "peripage_ble.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"

typedef struct { ble_addr_t address; bool service, name; } peripage_candidate_t;
typedef struct {
    peripage_candidate_t candidates[4];
    unsigned count;
    bool overflow, bound;
    ble_addr_t preferred;
} peripage_scan_t;

bool peripage_binding_load(peripage_scan_t *scan);
bool peripage_binding_save(const ble_addr_t *address);
void peripage_scan_observe(peripage_scan_t *scan, const ble_addr_t *address,
                           const struct ble_hs_adv_fields *fields);
peripage_result_t peripage_scan_select(const peripage_scan_t *scan, ble_addr_t *address);
bool peripage_profile_valid(const peripage_info_t *info);
