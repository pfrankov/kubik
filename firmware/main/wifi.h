// Wi-Fi: the station on the saved network, plus the setup access point that
// runs beside it (AP+STA) while the owner configures Kubik from a phone.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "device_status.h"
void wifi_status(device_status_t *status);

void wifi_init(void);
void wifi_start_sta(void);  // join the saved network (no-op without one)
void wifi_radio_stop(void);  // intentional battery poll sleep; also blocks link-manager restarts
void wifi_radio_resume(bool polling);  // allow the link manager to start the station again
bool wifi_sta_connected(void);
bool wifi_radio_started(void);
bool wifi_polling(void);
bool wifi_poll_join_expired(void);
bool wifi_link_attempt_allowed(void);
void wifi_link_attempt_started(void);
bool wifi_take_recovery_request(void);  // saved network lost for 60 s, at most once per 60 s
void wifi_set_fast(bool fast);  // no modem power save (during a conversation)
void wifi_set_doze(bool doze);  // screen off: the deepest modem sleep that keeps the link
bool wifi_time_ready(void);  // certificate validation requires synchronized wall time

// Station trial for setup: join `ssid` without saving it. NULL returns to the
// saved network. wifi_attempts() counts failed attempts since the last call.
void wifi_try(const char *ssid, const char *password);
int wifi_attempts(void);
uint8_t wifi_last_reason(void);  // wifi_err_reason_t of the last disconnect

// Setup access point (WPA2). Clients = phones currently associated.
void wifi_ap_start(const char *ssid, const char *password);
void wifi_ap_stop(void);
bool wifi_ap_active(void);
int wifi_ap_clients(void);

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_net_t;
// Blocking scan (~2 s), strongest first, duplicates merged. Returns the count.
int wifi_scan(wifi_net_t *out, int cap);
