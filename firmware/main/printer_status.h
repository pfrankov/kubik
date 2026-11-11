#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PRINTER_STATUS_UNCHECKED, PRINTER_STATUS_VERIFIED, PRINTER_STATUS_UNAVAILABLE
} printer_status_state_t;

typedef struct {
    printer_status_state_t state;
    char name[32];
    int battery_pct; // -1: unknown; retained last known reading is marked stale.
    int charging; // -1: unknown. No charging flag is proven for this printer.
    int64_t last_checked_ms, last_verified_ms;
    bool stale, busy, can_refresh;
    char error[32];
} printer_status_t;

// Thread-safe RAM snapshot only; no Bluetooth, NVS or agent traffic.
void printer_status_snapshot(printer_status_t *out);
// UI visibility is an advisory flag; the app worker owns all bounded polling.
void printer_status_set_visible(bool visible);
// One read-only refresh, rejected while busy/offline/Live/Muse or in backoff.
// Requests belong to the current authenticated session and are never replayed.
// A deliberate refresh may try after an automatic-search fault; it never clears that guard.
bool printer_status_refresh(void);
