#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[32], firmware[32], hardware[32];
    uint16_t dpi, width_dots, mtu;
    uint8_t battery_pct;
} peripage_info_t;
typedef enum {
    PERIPAGE_OK, PERIPAGE_NOT_FOUND, PERIPAGE_AMBIGUOUS,
    PERIPAGE_UNSUPPORTED, PERIPAGE_FAILED, PERIPAGE_BUSY, PERIPAGE_CANCELLED
} peripage_result_t;

// One worker owns all calls. Caller holds its audio/wake resource lease.
peripage_result_t peripage_open(peripage_info_t *info, uint32_t timeout_ms);
bool peripage_write_packet(const uint8_t *data, size_t size, uint32_t timeout_ms);
// Includes one 20 ms pace per packet, 3 s final check and 3 s scheduler reserve.
// Saturates at UINT32_MAX; the caller rejects an insufficient remaining budget.
uint32_t peripage_packet_stream_budget_ms(size_t packet_count);
// Fresh safe GET checks ordered ATT progress and the current connection only.
// Write Commands can be discarded: this never acknowledges raster or paper.
bool peripage_finish_packet_stream(uint32_t timeout_ms);
// UI may cancel the current operation; only the owning worker touches NimBLE.
void peripage_cancel(void);
// False means shutdown or persistent binding failed; never start another stack.
bool peripage_close(void);
// Rejects an active printer operation; unrelated Muse pairing may reset data.
bool peripage_forget_binding(void);
