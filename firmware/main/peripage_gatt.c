#include "peripage_gatt.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"

#if CONFIG_BT_NIMBLE_EATT_CHAN_NUM > 0
#error "PeriPage packet ordering requires the single fixed ATT bearer"
#endif

static uint32_t gatt_epoch;
static uint16_t gatt_connection, service_start, service_end, notify_handle, notify_end, write_handle, cccd_handle, packet_mtu;
static StaticSemaphore_t operation_storage, response_storage;
static SemaphoreHandle_t operation_done, response_ready;
static atomic_uint operation_next, operation_token;
static atomic_uint operation_completed;
static atomic_int operation_result;
static unsigned service_count;
static portMUX_TYPE response_mux = portMUX_INITIALIZER_UNLOCKED;
static bool response_armed, response_invalid;
static uint8_t response_bytes[32];
static size_t response_size;

static TickType_t time_left(int64_t end) {
    int64_t ms = (end - esp_timer_get_time()) / 1000;
    return ms > 0 ? pdMS_TO_TICKS(ms) : 0;
}

static void *operation_begin(void) {
    unsigned token = atomic_fetch_add(&operation_next, 1) + 1;
    if (!token) token = atomic_fetch_add(&operation_next, 1) + 1;
    atomic_store(&operation_token, token);
    atomic_store(&operation_completed, 0);
    while (xSemaphoreTake(operation_done, 0) == pdTRUE) {}
    operation_result = BLE_HS_ETIMEOUT;
    return (void *)(uintptr_t)token;
}

static bool operation_current(uint16_t connection, const void *argument) {
    return connection == gatt_connection && peripage_link_current(gatt_epoch) &&
        (uintptr_t)argument == atomic_load(&operation_token);
}

static void operation_finish(int result) {
    operation_result = result;
    atomic_store(&operation_completed, atomic_load(&operation_token));
    xSemaphoreGive(operation_done);
}

static bool operation_wait(int started, int64_t deadline) {
    unsigned token = atomic_load(&operation_token);
    if (started) return false;
    while (peripage_link_current(gatt_epoch)) {
        if (atomic_load(&operation_completed) == token) return !operation_result;
        if (xSemaphoreTake(operation_done, time_left(deadline)) != pdTRUE) return false;
    }
    return false;
}

void peripage_gatt_init(void) {
    if (!operation_done) operation_done = xSemaphoreCreateBinaryStatic(&operation_storage);
    if (!response_ready) response_ready = xSemaphoreCreateBinaryStatic(&response_storage);
}

void peripage_gatt_begin(uint32_t epoch, uint16_t connection) {
    gatt_epoch = epoch; gatt_connection = connection;
    service_start = service_end = notify_handle = notify_end = write_handle = cccd_handle = 0;
    service_count = 0; packet_mtu = 23;
    response_armed = response_invalid = false; response_size = 0;
    atomic_store(&operation_token, 0);
}

void peripage_gatt_lost(void) {
    // A stale cancellation wake is harmless: waits validate epoch/completion.
    if (operation_done) xSemaphoreGive(operation_done);
    if (response_ready) xSemaphoreGive(response_ready);
}

static int mtu_received(uint16_t connection, const struct ble_gatt_error *error, uint16_t mtu, void *argument) {
    if (!operation_current(connection, argument)) return 0;
    if (!error->status) packet_mtu = mtu;
    operation_finish(error->status);
    return 0;
}

static int service_received(uint16_t connection, const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *service, void *argument) {
    if (!operation_current(connection, argument)) return 0;
    if (!error->status) {
        service_count++; service_start = service->start_handle; service_end = service->end_handle;
    } else operation_finish(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}

static int characteristic_received(uint16_t connection, const struct ble_gatt_error *error,
                                   const struct ble_gatt_chr *characteristic, void *argument) {
    if (!operation_current(connection, argument)) return 0;
    if (error->status) { operation_finish(error->status == BLE_HS_EDONE ? 0 : error->status); return 0; }
    if (notify_handle && !notify_end) notify_end = characteristic->def_handle - 1;
    uint16_t uuid = ble_uuid_u16(&characteristic->uuid.u);
    if (uuid == 0xff01 && (characteristic->properties & BLE_GATT_CHR_PROP_NOTIFY)) notify_handle = characteristic->val_handle;
    const unsigned required = BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    if (uuid == 0xff02 && (characteristic->properties & required) == required) write_handle = characteristic->val_handle;
    return 0;
}

static int descriptor_received(uint16_t connection, const struct ble_gatt_error *error, uint16_t value_handle,
                               const struct ble_gatt_dsc *descriptor, void *argument) {
    (void)value_handle;
    if (!operation_current(connection, argument)) return 0;
    if (error->status) { operation_finish(error->status == BLE_HS_EDONE ? 0 : error->status); return 0; }
    if (ble_uuid_u16(&descriptor->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) cccd_handle = descriptor->handle;
    return 0;
}

static int written(uint16_t connection, const struct ble_gatt_error *error, struct ble_gatt_attr *attribute, void *argument) {
    (void)attribute;
    if (operation_current(connection, argument)) operation_finish(error->status);
    return 0;
}

static bool discover_attributes(int64_t deadline) {
    void *token = operation_begin();
    if (!operation_wait(ble_gattc_disc_svc_by_uuid(gatt_connection, BLE_UUID16_DECLARE(0xff00), service_received, token), deadline)) return false;
    if (service_count != 1) return false;
    token = operation_begin();
    if (!operation_wait(ble_gattc_disc_all_chrs(gatt_connection, service_start, service_end, characteristic_received, token), deadline)) return false;
    if (!notify_handle || !write_handle) return false;
    if (!notify_end) notify_end = service_end;
    token = operation_begin();
    return operation_wait(ble_gattc_disc_all_dscs(gatt_connection, notify_handle, notify_end, descriptor_received, token), deadline) && cccd_handle;
}

static bool write_attribute(uint16_t handle, const uint8_t *data, size_t size, int64_t deadline) {
    if (!peripage_link_current(gatt_epoch) || !data || !size || size > packet_mtu - 3u) return false;
    void *token = operation_begin();
    return operation_wait(ble_gattc_write_flat(gatt_connection, handle, data, size, written, token), deadline);
}

bool peripage_gatt_write(const uint8_t *data, size_t size, int64_t deadline) {
    if (!write_handle || !peripage_link_current(gatt_epoch) || !data || !size ||
        size > packet_mtu - 3u || esp_timer_get_time() >= deadline) return false;
    // NimBLE copies the caller's bytes into an owned mbuf before returning.
    // A zero result means queued locally, never accepted by the print engine.
    return ble_gattc_write_no_rsp_flat(gatt_connection, write_handle, data, size) == 0;
}

void peripage_gatt_notify(uint16_t handle, const struct os_mbuf *data) {
    if (handle != notify_handle || !peripage_link_current(gatt_epoch)) return;
    bool signal = false;
    portENTER_CRITICAL(&response_mux);
    if (response_armed) {
        size_t size = OS_MBUF_PKTLEN(data);
        if (!size || response_size + size > sizeof response_bytes) response_invalid = true;
        else {
            uint16_t copied = 0;
            if (ble_hs_mbuf_to_flat(data, response_bytes + response_size, size, &copied) || copied != size)
                response_invalid = true;
            else response_size += size;
        }
        signal = true;
    }
    portEXIT_CRITICAL(&response_mux);
    if (signal) xSemaphoreGive(response_ready);
}

static bool wait_response(int64_t deadline) {
    while (peripage_link_current(gatt_epoch)) {
        portENTER_CRITICAL(&response_mux);
        bool ready = response_size || response_invalid;
        portEXIT_CRITICAL(&response_mux);
        if (ready) return true;
        if (xSemaphoreTake(response_ready, time_left(deadline)) != pdTRUE) return false;
    }
    return false;
}

static bool query(uint8_t high, uint8_t low, uint8_t *result, size_t *size, int64_t deadline) {
    while (xSemaphoreTake(response_ready, 0) == pdTRUE) {}
    portENTER_CRITICAL(&response_mux);
    response_size = 0; response_invalid = false; response_armed = true;
    portEXIT_CRITICAL(&response_mux);
    const uint8_t command[] = {0x10, 0xff, high, low};
    bool ok = write_attribute(write_handle, command, sizeof command, deadline) && wait_response(deadline);
    // Known replies fit in one notification at the minimum ATT MTU. Reject
    // overflow / malformed responses instead of concatenating unrelated replies.
    portENTER_CRITICAL(&response_mux);
    ok = ok && !response_invalid && response_size && response_size <= *size;
    if (ok) { *size = response_size; memcpy(result, response_bytes, response_size); }
    response_armed = false;
    portEXIT_CRITICAL(&response_mux);
    return ok && peripage_link_current(gatt_epoch);
}

static bool query_string(uint8_t high, uint8_t low, char destination[32], int64_t deadline) {
    uint8_t bytes[31]; size_t size = sizeof bytes;
    if (!query(high, low, bytes, &size, deadline)) return false;
    for (size_t i = 0; i < size; i++) if (bytes[i] < 0x20 || bytes[i] > 0x7e) return false;
    memcpy(destination, bytes, size); destination[size] = 0;
    return true;
}

bool peripage_gatt_finish(uint8_t *battery_pct, int64_t deadline) {
    uint8_t battery[2]; size_t size = sizeof battery;
    if (!battery_pct || !query(0x50, 0xf1, battery, &size, deadline) ||
        size != 2 || battery[0] || battery[1] > 100) return false;
    *battery_pct = battery[1];
    return true;
}

peripage_result_t peripage_gatt_setup(peripage_info_t *info, int64_t deadline) {
    void *token = operation_begin();
    if (!operation_wait(ble_gattc_exchange_mtu(gatt_connection, mtu_received, token), deadline)) return PERIPAGE_FAILED;
    if (packet_mtu < 75) return PERIPAGE_UNSUPPORTED;
    if (!discover_attributes(deadline)) return PERIPAGE_UNSUPPORTED;
    const uint8_t subscribe[] = {1, 0};
    if (!write_attribute(cccd_handle, subscribe, sizeof subscribe, deadline)) return PERIPAGE_FAILED;
    if (!query_string(0x20, 0xf1, info->firmware, deadline) ||
        !query_string(0x30, 0x10, info->hardware, deadline) ||
        !query_string(0x30, 0x11, info->name, deadline)) return PERIPAGE_FAILED;
    uint8_t battery[2]; size_t size = sizeof battery;
    if (!query(0x50, 0xf1, battery, &size, deadline) || size != 2 || battery[0] || battery[1] > 100) return PERIPAGE_FAILED;
    info->battery_pct = battery[1]; info->charging = -1; info->mtu = packet_mtu;
    return PERIPAGE_OK;
}
