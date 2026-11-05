#include "frame_pacing_s3.h"
#include "display_te_s3.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int64_t s_accepted_edge, s_next_slot, s_expected_edge;

static bool clock_ready(const display_te_clock_t *clock, int64_t now) {
    return clock->period_us && clock->accepted_edge_us > 0 && now >= clock->latest_edge_us &&
           now - clock->latest_edge_us <= (int64_t)clock->period_us * 2;
}

static display_te_clock_t measured_clock(void) {
    display_te_clock_t clock;
    for (int attempt = 0; attempt < 2; attempt++) {
        display_te_clock_snapshot(&clock);
        if (clock_ready(&clock, esp_timer_get_time())) return clock;
        ESP_ERROR_CHECK(display_te_wait());
    }
    display_te_clock_snapshot(&clock);
    ESP_ERROR_CHECK(clock_ready(&clock, esp_timer_get_time()) ? ESP_OK : ESP_ERR_INVALID_STATE);
    return clock;
}

static void wait_until(int64_t target, int64_t now) {
    if (now >= target) { vTaskDelay(1); return; }
    const int64_t tick_us = portTICK_PERIOD_MS * 1000;
    TickType_t ticks = (TickType_t)((target - now + tick_us - 1) / tick_us);
    // Notifications are coalesced once at the slot, not used as a polling loop.
    // PWR/brightness state is applied by the next frame, at most one period later.
    vTaskDelay(ticks);
    // vTaskDelay is relative to the current kernel tick, not our microsecond
    // clock. One final tick covers an early wake due to that fractional phase.
    if (esp_timer_get_time() < target) vTaskDelay(1);
}

esp_err_t frame_pacing_s3_sync(void) {
    for (int attempt = 0; attempt < 2; attempt++) {
        esp_err_t error = display_te_wait();
        if (error != ESP_OK) return error;
        if (!s_expected_edge) return ESP_OK;
        display_te_clock_t clock;
        display_te_clock_snapshot(&clock);
        if (!clock.period_us) return ESP_ERR_INVALID_STATE;
        if (clock.accepted_edge_us >= s_expected_edge - clock.period_us / 2) return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

bool frame_pacing_s3_wait(bool powered) {
    if (!powered) {
        s_accepted_edge = s_next_slot = s_expected_edge = 0;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        return false;
    }
    display_te_clock_t clock = measured_clock();
    int64_t period = clock.period_us;
    // Compare before reanchoring: an accepted edge can itself reveal a skipped
    // presentation. Round to the nearest measured slot to tolerate period drift.
    bool late_edge = s_expected_edge && clock.accepted_edge_us != s_accepted_edge &&
                     clock.accepted_edge_us > s_expected_edge + period / 2;
    if (clock.accepted_edge_us != s_accepted_edge) {
        s_accepted_edge = clock.accepted_edge_us;
        s_next_slot = s_accepted_edge + period;
    }
    int64_t now = esp_timer_get_time(), slot = s_next_slot;
    bool missed = late_edge || now >= slot + period;
    wait_until(slot, now);
    ulTaskNotifyTake(pdTRUE, 0);
    s_next_slot = slot + 2 * period;
    now = esp_timer_get_time();
    s_expected_edge = slot + period;
    // A CPU overrun was already counted above. The next frame can only target
    // a future physical edge; do not count the same expired deadline twice.
    if (now >= s_expected_edge)
        s_expected_edge = clock.latest_edge_us +
                          ((now - clock.latest_edge_us) / period + 1) * period;
    if (now >= s_next_slot)
        s_next_slot += ((now - s_next_slot) / (2 * period) + 1) * (2 * period);
    return missed;
}
