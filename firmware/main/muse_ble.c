// Native Muse v5 framing. Cryptography lives in the pinned upstream component.
#include "muse_ble.h"
#include "muse_store.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "link_pairing.h"

#define RX_CAP 8192
#define CHUNK_MAX 160
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x0c,0x24,0x5f,0xcf,0x4e,0x31,0x46,0x8b,0xcf,0x46,0xea,0x38,0x1c,0x3d,0xdd,0x7f);
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(
    0x01,0x9b,0x8f,0x5e,0x2d,0x3c,0xf0,0xa1,0x6e,0x4a,0xa2,0x28,0x29,0x30,0x59,0x4d);
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(
    0x6c,0x5b,0x4a,0x3f,0x2e,0x1d,0x0a,0x8f,0x9c,0x4e,0x2b,0x7b,0xca,0xc4,0x5d,0xd7);
static atomic_uint connection_epoch;
static atomic_uint connection = BLE_HS_CONN_HANDLE_NONE;
static atomic_bool subscribed;
static atomic_uint mtu = 23;
static uint16_t tx_handle;
static const char *device_name;
static muse_ble_command_t dispatch;
static QueueHandle_t commands;
static SemaphoreHandle_t tx_lock;
static uint8_t *rx;
static size_t rx_size;
static unsigned rx_total, rx_next;
typedef struct { char *json; uint32_t epoch; } command_t;

static void rx_reset(void) {
    if (rx) { muse_store_wipe(rx, RX_CAP); free(rx); }
    rx = NULL; rx_size = rx_total = rx_next = 0;
}

bool muse_ble_current(uint32_t epoch) {
    return epoch == atomic_load(&connection_epoch) && atomic_load(&connection) != BLE_HS_CONN_HANDLE_NONE;
}

static bool queue_command(const uint8_t *bytes, size_t size) {
    if (!size || size >= RX_CAP || memchr(bytes, 0, size)) return false;
    command_t command = {.json = malloc(size + 1), .epoch = atomic_load(&connection_epoch)};
    if (!command.json) return false;
    memcpy(command.json, bytes, size); command.json[size] = 0;
    if (xQueueSend(commands, &command, 0) == pdTRUE) return true;
    muse_store_wipe(command.json, size); free(command.json);
    return false;
}

static bool receive(const uint8_t *bytes, size_t size) {
    if (size < 3 || bytes[0] != 0xfe) { rx_reset(); return queue_command(bytes, size); }
    unsigned index = bytes[1], total = bytes[2];
    if (!total || index >= total) { rx_reset(); return false; }
    if (!index) {
        rx_reset(); rx = malloc(RX_CAP); rx_total = total;
        if (!rx) return false;
    }
    if (!rx || total != rx_total || index != rx_next || rx_size + size - 3 >= RX_CAP) {
        rx_reset(); return false;
    }
    memcpy(rx + rx_size, bytes + 3, size - 3); rx_size += size - 3; rx_next++;
    if (rx_next != rx_total) return true;
    bool queued = queue_command(rx, rx_size);
    rx_reset(); return queued;
}

static int access_rx(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *context, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    uint8_t bytes[512]; uint16_t size = 0;
    if (ble_hs_mbuf_to_flat(context->om, bytes, sizeof bytes, &size)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    bool ok = receive(bytes, size);
    muse_store_wipe(bytes, sizeof bytes);
    return ok ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_tx(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *context, void *arg) {
    (void)conn; (void)attr; (void)arg;
    return context->op == BLE_GATT_ACCESS_OP_READ_CHR ? 0 : BLE_ATT_ERR_WRITE_NOT_PERMITTED;
}

static const struct ble_gatt_chr_def characteristics[] = {
    {.uuid=&rx_uuid.u,.access_cb=access_rx,.flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP},
    {.uuid=&tx_uuid.u,.access_cb=access_tx,.flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY,.val_handle=&tx_handle},
    {0}
};
static const struct ble_gatt_svc_def services[] = {
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&service_uuid.u,.characteristics=characteristics}, {0}
};
static void advertise(void);
static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) { advertise(); break; }
        atomic_fetch_add(&connection_epoch, 1); atomic_store(&connection, event->connect.conn_handle);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        atomic_store(&connection, BLE_HS_CONN_HANDLE_NONE); atomic_fetch_add(&connection_epoch, 1);
        atomic_store(&subscribed, false); atomic_store(&mtu, 23); rx_reset(); link_pairing_reset(); advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE: atomic_store(&subscribed, event->subscribe.cur_notify); break;
    case BLE_GAP_EVENT_MTU: atomic_store(&mtu, event->mtu.value); break;
    default: break;
    }
    return 0;
}

static void advertise(void) {
    uint8_t manufacturer[] = {0xff, 0xff, 0};
    struct ble_hs_adv_fields fields = {.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128=(ble_uuid128_t *)&service_uuid,.num_uuids128=1,.uuids128_is_complete=1,
        .mfg_data=manufacturer,.mfg_data_len=sizeof manufacturer};
    struct ble_hs_adv_fields response = {.name=(uint8_t *)device_name,
        .name_len=strlen(device_name),.name_is_complete=1};
    struct ble_gap_adv_params params = {.conn_mode=BLE_GAP_CONN_MODE_UND,.disc_mode=BLE_GAP_DISC_MODE_GEN};
    if (ble_gap_adv_set_fields(&fields) || ble_gap_adv_rsp_set_fields(&response)) return;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
}
static void synced(void) { if (!ble_hs_util_ensure_addr(0)) advertise(); }
static void host_task(void *arg) { (void)arg; nimble_port_run(); nimble_port_freertos_deinit(); }
static void worker(void *arg) {
    (void)arg; command_t command;
    for (;;) {
        if (xQueueReceive(commands, &command, portMAX_DELAY) != pdTRUE) continue;
        if (muse_ble_current(command.epoch)) dispatch(command.json, false, command.epoch);
        muse_store_wipe(command.json, strlen(command.json)); free(command.json);
    }
}

esp_err_t muse_ble_start(const char *name, muse_ble_command_t handler) {
    device_name = name; dispatch = handler;
    commands = xQueueCreate(2, sizeof(command_t)); tx_lock = xSemaphoreCreateMutex();
    if (!commands || !tx_lock) return ESP_ERR_NO_MEM;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_svc_gap_init(); ble_svc_gatt_init();
    if (ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) return ESP_FAIL;
    ble_svc_gap_device_name_set(name); ble_hs_cfg.sync_cb = synced;
    if (xTaskCreate(worker, "muse_pair", 6144, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool muse_ble_send(const char *json, uint32_t generation) {
    if (!tx_lock || !json) return false;
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    uint32_t epoch = atomic_load(&connection_epoch);
    unsigned maximum = atomic_load(&mtu) - 3;
    if (maximum > CHUNK_MAX) maximum = CHUNK_MAX;
    size_t size = strlen(json), usable = maximum - 3, total = (size + usable - 1) / usable;
    bool ok = total && total <= 255;
    for (unsigned i = 0; ok && i < total; i++) {
        ok = muse_ble_current(epoch) && atomic_load(&subscribed) &&
            (!generation || link_pairing_record_session_is_current(generation));
        if (!ok) break;
        size_t offset = i * usable, amount = size - offset;
        if (amount > usable) amount = usable;
        uint8_t bytes[CHUNK_MAX] = {0xfe, i, total};
        memcpy(bytes + 3, json + offset, amount);
        struct os_mbuf *mbuf = ble_hs_mbuf_from_flat(bytes, amount + 3);
        ok = mbuf && ble_gatts_notify_custom(atomic_load(&connection), tx_handle, mbuf) == 0;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    xSemaphoreGive(tx_lock); return ok;
}
void muse_ble_disconnect(void) {
    unsigned handle = atomic_load(&connection);
    if (handle != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
}
