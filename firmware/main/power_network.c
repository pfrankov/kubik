#include "power_network.h"

void power_network_init(power_network_t *state, int64_t now_ms) {
    state->phase = POWER_NETWORK_ACTIVE;
    state->since_ms = now_ms;
    state->next_check_ms = 0;
}

static power_network_result_t result(power_network_action_t action, power_network_phase_t phase) {
    return (power_network_result_t){
        .action = action,
        .scheduled = phase != POWER_NETWORK_ACTIVE,
        .checking = phase == POWER_NETWORK_CHECK,
    };
}

static bool sleep_allowed(const power_network_inputs_t *inputs) {
    return inputs->screen_dark && inputs->usb_absent_known && !inputs->charging &&
           !inputs->setup_active && !inputs->access_point_active && !inputs->usb_host_active &&
           !inputs->work_active && inputs->saved_networks;
}

power_network_result_t power_network_step(power_network_t *state, int64_t now_ms,
                                          const power_network_inputs_t *inputs) {
    power_network_action_t action = POWER_NETWORK_NO_ACTION;
    bool eligible = sleep_allowed(inputs);
    if (state->phase != POWER_NETWORK_ACTIVE && !eligible) {
        state->phase = POWER_NETWORK_ACTIVE;
        state->since_ms = now_ms;
        state->next_check_ms = 0;
        action = POWER_NETWORK_RADIO_ON;
    } else if (state->phase == POWER_NETWORK_ACTIVE && eligible) {
        state->phase = POWER_NETWORK_SLEEP;
        state->since_ms = now_ms;
        state->next_check_ms = now_ms + POWER_NETWORK_SLEEP_MS;
        action = POWER_NETWORK_RADIO_OFF;
    } else if (state->phase == POWER_NETWORK_SLEEP && now_ms >= state->next_check_ms) {
        state->phase = POWER_NETWORK_CHECK;
        state->since_ms = now_ms;
        state->next_check_ms += POWER_NETWORK_POLL_MS;
        action = POWER_NETWORK_RADIO_ON;
    } else if (state->phase == POWER_NETWORK_CHECK && now_ms - state->since_ms >= POWER_NETWORK_CHECK_MS) {
        state->phase = POWER_NETWORK_SLEEP;
        state->since_ms = now_ms;
        action = POWER_NETWORK_RADIO_OFF;
    }
    return result(action, state->phase);
}

bool power_network_scheduled(const power_network_t *state) {
    return state->phase != POWER_NETWORK_ACTIVE;
}

bool power_network_checking(const power_network_t *state) {
    return state->phase == POWER_NETWORK_CHECK;
}
