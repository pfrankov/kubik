#include "peripage_profile.h"
#include "ble_runtime.h"
#include "app_nvs.h"

#include <string.h>
#include "nvs.h"

static bool address_equal(const ble_addr_t *a, const ble_addr_t *b) {
    return a->type == b->type && !memcmp(a->val, b->val, sizeof a->val);
}

static bool binding_load_locked(peripage_scan_t *scan) {
    nvs_handle_t handle;
    if (!app_nvs_ready_locked()) return false;
    esp_err_t result = nvs_open("kubik", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    uint8_t bytes[7]; size_t size = sizeof bytes;
    result = nvs_get_blob(handle, "printer_peer", bytes, &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK || size != sizeof bytes || bytes[0] > BLE_ADDR_RANDOM) return false;
    scan->preferred.type = bytes[0]; memcpy(scan->preferred.val, bytes + 1, 6);
    scan->bound = true;
    return true;
}

bool peripage_binding_load(peripage_scan_t *scan) {
    if (app_nvs_lock() != ESP_OK) return false;
    bool ok = binding_load_locked(scan);
    app_nvs_unlock();
    return ok;
}

static bool binding_saved_locked(bool ok) {
    if (!ok && app_nvs_recover_locked() != ESP_OK) app_nvs_mark_unhealthy_locked();
    app_nvs_unlock();
    return ok;
}

bool peripage_binding_save(const ble_addr_t *address) {
    if (!ble_runtime_owned(BLE_RUNTIME_NONE)) return false;
    if (app_nvs_lock() != ESP_OK) return false;
    nvs_handle_t handle;
    if (!app_nvs_ready_locked() || nvs_open("kubik", NVS_READWRITE, &handle) != ESP_OK) {
        app_nvs_unlock(); return false;
    }
    uint8_t bytes[7]; bytes[0] = address->type; memcpy(bytes + 1, address->val, 6);
    bool ok = nvs_set_blob(handle, "printer_peer", bytes, sizeof bytes) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return binding_saved_locked(ok);
}

bool peripage_forget_binding(void) {
    // A reset may also run during Muse pairing; only a printer operation can
    // still commit this namespace. Existing Muse NVS writes use flash suspend.
    if (ble_runtime_owned(BLE_RUNTIME_PRINTER)) return false;
    if (app_nvs_lock() != ESP_OK) return false;
    nvs_handle_t handle;
    if (!app_nvs_ready_locked() || nvs_open("kubik", NVS_READWRITE, &handle) != ESP_OK) {
        app_nvs_unlock(); return false;
    }
    esp_err_t result = nvs_erase_key(handle, "printer_peer");
    if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    bool ok = result == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return binding_saved_locked(ok);
}

static bool printer_name(const struct ble_hs_adv_fields *fields) {
    return fields->name_len >= 9 && fields->name_len < 32 && !memcmp(fields->name, "PeriPage+", 9);
}

static bool printer_service(const struct ble_hs_adv_fields *fields) {
    for (unsigned i = 0; i < fields->num_uuids16; i++)
        if (fields->uuids16[i].value == 0xfee7) return true;
    return false;
}

void peripage_scan_observe(peripage_scan_t *scan, const ble_addr_t *address,
                           const struct ble_hs_adv_fields *fields) {
    bool name = printer_name(fields), service = printer_service(fields);
    if (!name && !service) return;
    unsigned index = 0;
    while (index < scan->count && !address_equal(&scan->candidates[index].address, address)) index++;
    if (index == scan->count) {
        if (scan->count == 4) { scan->overflow = true; return; }
        scan->candidates[scan->count++].address = *address;
    }
    scan->candidates[index].name |= name;
    scan->candidates[index].service |= service;
}

peripage_result_t peripage_scan_select(const peripage_scan_t *scan, ble_addr_t *address) {
    if (scan->overflow) return PERIPAGE_AMBIGUOUS;
    unsigned matches = 0;
    for (unsigned i = 0; i < scan->count; i++) {
        if (!scan->candidates[i].name || !scan->candidates[i].service) continue;
        *address = scan->candidates[i].address; matches++;
    }
    if (!matches) return PERIPAGE_NOT_FOUND;
    return matches == 1 ? PERIPAGE_OK : PERIPAGE_AMBIGUOUS;
}

bool peripage_profile_valid(const peripage_info_t *info) {
    size_t n = strlen(info->firmware);
    return !strncmp(info->name, "PeriPage+", 9) && info->firmware[0] == 'V' &&
        n > 7 && !strcmp(info->firmware + n - 7, "_304dpi") && info->hardware[0] &&
        info->battery_pct <= 100 && info->mtu >= 75;
}
