#include "peripage_ble.h"

#include <stdatomic.h>
#include <string.h>
#include "ble_runtime.h"
#include "peripage_gatt.h"
#include "peripage_profile.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define SCAN_MS 3000
#define PACKET_PACE_MS 20u
#define FINISH_MS 3000u
#define SCHEDULER_RESERVE_MS 3000u
#define CANCELLED_BIT (1u << 31)
static atomic_uint link_epoch;
static atomic_uint operation_state;
static atomic_bool link_lost;
static atomic_uint connection = BLE_HS_CONN_HANDLE_NONE;
static uint8_t own_address_type;
static StaticSemaphore_t event_storage;
static SemaphoreHandle_t event_ready;
static atomic_int event_result;
static atomic_bool event_completed;
static bool radio_started, binding_pending;
static ble_addr_t selected_address;
static peripage_scan_t scan_state;
static uint16_t negotiated_mtu;

bool peripage_link_current(uint32_t epoch) {
    return epoch == atomic_load(&link_epoch) && atomic_load(&operation_state) == epoch && !atomic_load(&link_lost) &&
        connection != BLE_HS_CONN_HANDLE_NONE && ble_runtime_owned(BLE_RUNTIME_PRINTER);
}

static bool callback_current(const void *argument) {
    return (uintptr_t)argument == atomic_load(&link_epoch) &&
        atomic_load(&operation_state) == (uintptr_t)argument && ble_runtime_owned(BLE_RUNTIME_PRINTER);
}

static void signal_event(int result) { event_result = result; atomic_store(&event_completed, true); xSemaphoreGive(event_ready); }

static void clear_event(void) {
    while (xSemaphoreTake(event_ready, 0) == pdTRUE) {}
    event_result = BLE_HS_ETIMEOUT;
    atomic_store(&event_completed, false);
}

static bool wait_event(int64_t deadline) {
    while (!(atomic_load(&operation_state) & CANCELLED_BIT)) {
        if (atomic_load(&event_completed)) return !event_result;
        int64_t ms = (deadline - esp_timer_get_time()) / 1000;
        if (ms <= 0 || xSemaphoreTake(event_ready, pdMS_TO_TICKS(ms)) != pdTRUE) return false;
    }
    return false;
}

static void observed(const struct ble_gap_disc_desc *discovery) {
    struct ble_hs_adv_fields fields;
    if (!ble_hs_adv_parse_fields(&fields, discovery->data, discovery->length_data))
        peripage_scan_observe(&scan_state, &discovery->addr, &fields);
}

static int printer_gap(struct ble_gap_event *event, void *argument) {
    if (!callback_current(argument)) return 0;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: observed(&event->disc); break;
    case BLE_GAP_EVENT_DISC_COMPLETE: signal_event(event->disc_complete.reason); break;
    case BLE_GAP_EVENT_CONNECT:
        if (!event->connect.status) connection = event->connect.conn_handle;
        signal_event(event->connect.status);
        break;
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
        // Complete the connection's feature/version procedures before allowing
        // another Link Layer control procedure from the peripheral.
        return connection == BLE_HS_CONN_HANDLE_NONE ? BLE_ERR_CONN_PARMS : 0;
    case BLE_GAP_EVENT_DISCONNECT:
        connection = BLE_HS_CONN_HANDLE_NONE; atomic_store(&link_lost, true);
        peripage_gatt_lost(); signal_event(BLE_HS_ENOTCONN);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.conn_handle == connection) peripage_gatt_notify(event->notify_rx.attr_handle, event->notify_rx.om);
        break;
    default: break;
    }
    return 0;
}

static void printer_synced(void) {
    unsigned state = atomic_load(&operation_state);
    if (!state || (state & CANCELLED_BIT)) return;
    int result = ble_hs_util_ensure_addr(0);
    if (!result) result = ble_hs_id_infer_auto(0, &own_address_type);
    signal_event(result);
}

static esp_err_t printer_prepare(void) {
    ble_svc_gap_init(); ble_svc_gatt_init();
    return ble_att_set_preferred_mtu(185) ? ESP_FAIL : ESP_OK;
}

static peripage_result_t find_printer(uint32_t epoch, int64_t deadline) {
    struct ble_gap_disc_params parameters = {.itvl = 0x30, .window = 0x30, .filter_duplicates = 1, .passive = 0};
    clear_event();
    int64_t remaining = (deadline - esp_timer_get_time()) / 1000;
    if (remaining <= SCAN_MS) return PERIPAGE_FAILED;
    if (ble_gap_disc(own_address_type, SCAN_MS, &parameters, printer_gap, (void *)(uintptr_t)epoch)) return PERIPAGE_FAILED;
    if (!wait_event(deadline)) return PERIPAGE_FAILED;
    return peripage_scan_select(&scan_state, &selected_address);
}

static peripage_result_t select_printer(uint32_t epoch, int64_t deadline) {
    if (!scan_state.bound) return find_printer(epoch, deadline);
    // The controller's bounded connect procedure scans for this verified peer.
    // Requiring a second complete advertisement would needlessly gate reconnect.
    selected_address = scan_state.preferred;
    return PERIPAGE_OK;
}

static peripage_result_t connect_printer(uint32_t epoch, peripage_info_t *info, int64_t deadline) {
    int64_t remaining = (deadline - esp_timer_get_time()) / 1000;
    if (remaining <= 0) return PERIPAGE_FAILED;
    clear_event();
    if (ble_gap_connect(own_address_type, &selected_address, remaining, NULL, printer_gap, (void *)(uintptr_t)epoch)) return PERIPAGE_FAILED;
    if (!wait_event(deadline) || connection == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW("printer_ble", "connect wait failed result=%d completed=%d handle=%u", event_result,
            atomic_load(&event_completed), atomic_load(&connection));
        return PERIPAGE_FAILED;
    }
    peripage_gatt_begin(epoch, connection);
    peripage_result_t result = peripage_gatt_setup(info, deadline);
    if (result != PERIPAGE_OK) return result;
    if (!peripage_profile_valid(info)) return PERIPAGE_UNSUPPORTED;
    info->dpi = 304; info->width_dots = 576; negotiated_mtu = info->mtu;
    binding_pending = !scan_state.bound;
    return PERIPAGE_OK;
}

static bool open_parameters(peripage_info_t *info, uint32_t timeout_ms) {
    return info && timeout_ms >= 4000 && timeout_ms <= 30000;
}

static bool radio_busy(void) {
    return radio_started || atomic_load(&operation_state) ||
        ble_runtime_owned(BLE_RUNTIME_MUSE) || ble_runtime_owned(BLE_RUNTIME_PRINTER);
}

peripage_result_t peripage_open(peripage_info_t *info, uint32_t timeout_ms) {
    if (!open_parameters(info, timeout_ms)) return PERIPAGE_FAILED;
    if (radio_busy()) return PERIPAGE_BUSY;
    if (!event_ready) event_ready = xSemaphoreCreateBinaryStatic(&event_storage);
    peripage_gatt_init();
    uint32_t epoch = (atomic_fetch_add(&link_epoch, 1) + 1) & ~CANCELLED_BIT;
    if (!epoch) epoch = 1;
    atomic_store(&link_epoch, epoch); atomic_store(&operation_state, epoch);
    memset(info, 0, sizeof *info); memset(&scan_state, 0, sizeof scan_state);
    if (!peripage_binding_load(&scan_state)) { atomic_store(&operation_state, 0); return PERIPAGE_FAILED; }
    if (atomic_load(&operation_state) & CANCELLED_BIT) { atomic_store(&operation_state, 0); return PERIPAGE_CANCELLED; }
    connection = BLE_HS_CONN_HANDLE_NONE; binding_pending = false; negotiated_mtu = 23;
    atomic_store(&link_lost, false);
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    clear_event();
    esp_err_t started = ble_runtime_start(BLE_RUNTIME_PRINTER, printer_prepare, printer_synced);
    if (started != ESP_OK) {
        atomic_store(&operation_state, 0);
        return started == ESP_ERR_INVALID_STATE ? PERIPAGE_BUSY : PERIPAGE_FAILED;
    }
    radio_started = true;
    peripage_result_t result = wait_event(deadline) ? select_printer(epoch, deadline) : PERIPAGE_FAILED;
    if (result == PERIPAGE_OK) result = connect_printer(epoch, info, deadline);
    if (atomic_load(&operation_state) & CANCELLED_BIT) result = PERIPAGE_CANCELLED;
    if (result != PERIPAGE_OK) peripage_close();
    return result;
}

void peripage_cancel(void) {
    unsigned state = atomic_load(&operation_state);
    if (!state || (state & CANCELLED_BIT)) return;
    if (!atomic_compare_exchange_strong(&operation_state, &state, state | CANCELLED_BIT)) return;
    // Never call controller APIs from UI concurrently with worker teardown.
    if (event_ready) xSemaphoreGive(event_ready);
    peripage_gatt_lost();
}

static bool pace_packet(int64_t deadline) {
    int64_t until = esp_timer_get_time() + PACKET_PACE_MS * 1000;
    if (until > deadline) return false;
    while (peripage_link_current(atomic_load(&link_epoch))) {
        int64_t remaining = until - esp_timer_get_time();
        if (remaining <= 0) return true;
        TickType_t ticks = pdMS_TO_TICKS((remaining + 999) / 1000);
        xSemaphoreTake(event_ready, ticks ? ticks : 1);
    }
    return false;
}

uint32_t peripage_packet_stream_budget_ms(size_t packet_count) {
    const uint32_t reserve = FINISH_MS + SCHEDULER_RESERVE_MS;
    if (packet_count > (UINT32_MAX - reserve) / PACKET_PACE_MS) return UINT32_MAX;
    return (uint32_t)packet_count * PACKET_PACE_MS + reserve;
}

bool peripage_write_packet(const uint8_t *data, size_t size, uint32_t timeout_ms) {
    if (!radio_started || timeout_ms < PACKET_PACE_MS || timeout_ms > 10000 || !size || size > negotiated_mtu - 3u) return false;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    bool ok = peripage_gatt_write(data, size, deadline) && pace_packet(deadline);
    if (!ok) atomic_store(&link_lost, true); // No second packet after an uncertain write.
    return ok;
}

bool peripage_finish_packet_stream(uint32_t timeout_ms) {
    if (!radio_started || !timeout_ms || timeout_ms > FINISH_MS) return false;
    bool ok = peripage_gatt_finish(esp_timer_get_time() + (int64_t)timeout_ms * 1000);
    if (!ok) atomic_store(&link_lost, true);
    return ok;
}

bool peripage_close(void) {
    atomic_store(&operation_state, 0);
    if (!radio_started) return !ble_runtime_owned(BLE_RUNTIME_PRINTER);
    binding_pending &= !atomic_load(&link_lost);
    // The host stop owns GAP cancellation and graceful disconnection. A prior
    // terminate makes its second terminate return EALREADY, which IDF treats as
    // already disconnected and skips waiting for the actual on-air disconnect.
    atomic_store(&link_lost, true); atomic_fetch_add(&link_epoch, 1);
    peripage_gatt_lost();
    esp_err_t stopped = ble_runtime_stop(BLE_RUNTIME_PRINTER);
    if (stopped != ESP_OK) { ESP_LOGE("printer_ble", "BLE shutdown failed: %d", (int)stopped); return false; }
    radio_started = false; connection = BLE_HS_CONN_HANDLE_NONE;
    bool saved = !binding_pending || peripage_binding_save(&selected_address);
    binding_pending = false;
    return saved;
}
