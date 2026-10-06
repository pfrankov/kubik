#include <assert.h>
#include <stdio.h>

#include "power_network.h"

static power_network_inputs_t idle_on_battery(void) {
    return (power_network_inputs_t){
        .screen_dark = true,
        .usb_absent_known = true,
        .saved_networks = true,
    };
}

int main(void) {
    power_network_t state;
    power_network_init(&state, 0);
    power_network_inputs_t input = idle_on_battery();
    power_network_result_t result = power_network_step(&state, 100, &input);
    assert(result.action == POWER_NETWORK_RADIO_OFF && result.scheduled && !result.checking);
    assert(power_network_step(&state, 12099, &input).action == POWER_NETWORK_NO_ACTION);
    result = power_network_step(&state, 12100, &input);
    assert(result.action == POWER_NETWORK_RADIO_ON && result.scheduled && result.checking);
    assert(power_network_step(&state, 60099, &input).action == POWER_NETWORK_NO_ACTION);
    result = power_network_step(&state, 60100, &input);
    assert(result.action == POWER_NETWORK_RADIO_OFF && result.scheduled && !result.checking);
    assert(power_network_step(&state, 72099, &input).action == POWER_NETWORK_NO_ACTION);
    result = power_network_step(&state, 72100, &input);
    assert(result.action == POWER_NETWORK_RADIO_ON && result.checking);
    result = power_network_step(&state, 120100, &input);
    assert(result.action == POWER_NETWORK_RADIO_OFF && result.scheduled);

    // The initial interval is also bounded: healthy join + discovery + TLS + wake fit one minute.
    int healthy_check = POWER_NETWORK_JOIN_MS + 3000 + 22000 + 2000;
    assert(healthy_check < POWER_NETWORK_CHECK_MS);
    assert(POWER_NETWORK_SLEEP_MS + healthy_check < POWER_NETWORK_POLL_MS);
    // Missing, unknown, or externally powered source resets the schedule immediately.
    input.usb_absent_known = false;
    result = power_network_step(&state, 145000, &input);
    assert(result.action == POWER_NETWORK_RADIO_ON && !result.scheduled);
    input = idle_on_battery();
    assert(power_network_step(&state, 146000, &input).action == POWER_NETWORK_RADIO_OFF);
    input.charging = true;
    result = power_network_step(&state, 147000, &input);
    assert(result.action == POWER_NETWORK_RADIO_ON && !result.scheduled);
    input = idle_on_battery();
    assert(power_network_step(&state, 148000, &input).action == POWER_NETWORK_RADIO_OFF);

    input.usb_host_active = true;
    result = power_network_step(&state, 149000, &input);
    assert(result.action == POWER_NETWORK_RADIO_ON && !result.scheduled);
    input = idle_on_battery();
    assert(power_network_step(&state, 150000, &input).action == POWER_NETWORK_RADIO_OFF);
    input.work_active = true;
    result = power_network_step(&state, 151000, &input);
    assert(result.action == POWER_NETWORK_RADIO_ON && !result.scheduled);

    // Work/setup/AP/awake states and an empty saved list never allow scheduled radio-off.
    input = idle_on_battery(); input.screen_dark = false;
    assert(power_network_step(&state, 152000, &input).action == POWER_NETWORK_NO_ACTION);
    input = idle_on_battery(); input.work_active = true;
    assert(power_network_step(&state, 153000, &input).action == POWER_NETWORK_NO_ACTION);
    input = idle_on_battery(); input.setup_active = true;
    assert(power_network_step(&state, 154000, &input).action == POWER_NETWORK_NO_ACTION);
    input = idle_on_battery(); input.access_point_active = true;
    assert(power_network_step(&state, 155000, &input).action == POWER_NETWORK_NO_ACTION);
    input = idle_on_battery(); input.saved_networks = false;
    assert(power_network_step(&state, 156000, &input).action == POWER_NETWORK_NO_ACTION);

    puts("power-network: 60 s cadence, first/off gap 12 s, bounded 48 s checks, PMIC/charger/USB/activity resets and work gates passed");
}
