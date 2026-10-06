#pragma once

#include <stdbool.h>
#include <stdint.h>

#define POWER_NETWORK_POLL_MS 60000
#define POWER_NETWORK_JOIN_MS 20000
// Join + discovery (3 s) + WebSocket (22 s) + delivery (2 s), with 1 s margin.
#define POWER_NETWORK_CHECK_MS 48000
#define POWER_NETWORK_SLEEP_MS (POWER_NETWORK_POLL_MS - POWER_NETWORK_CHECK_MS)

typedef enum {
    POWER_NETWORK_ACTIVE,
    POWER_NETWORK_SLEEP,
    POWER_NETWORK_CHECK,
} power_network_phase_t;

typedef enum {
    POWER_NETWORK_NO_ACTION,
    POWER_NETWORK_RADIO_OFF,
    POWER_NETWORK_RADIO_ON,
} power_network_action_t;

typedef struct {
    power_network_phase_t phase;
    int64_t since_ms;
    int64_t next_check_ms;
} power_network_t;

typedef struct {
    bool screen_dark;
    bool usb_absent_known;
    bool charging;
    bool setup_active;
    bool access_point_active;
    bool usb_host_active;
    bool work_active;
    bool saved_networks;
} power_network_inputs_t;

typedef struct {
    power_network_action_t action;
    bool scheduled;
    bool checking;
} power_network_result_t;

void power_network_init(power_network_t *state, int64_t now_ms);
power_network_result_t power_network_step(power_network_t *state, int64_t now_ms,
                                          const power_network_inputs_t *inputs);
bool power_network_scheduled(const power_network_t *state);
bool power_network_checking(const power_network_t *state);
