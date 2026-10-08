#include "input_probe.h"
#include "board.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct { int x, y; bool down; int64_t until; } s_sample;

bool input_probe_sample(bool down, int x, int y) {
    if (x < 0 || x >= 480 || y < 0 || y >= 480) return false;
    int64_t until = esp_timer_get_time() + 350000;
    portENTER_CRITICAL(&s_lock);
    s_sample.x = x; s_sample.y = y; s_sample.down = down; s_sample.until = until;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool input_probe_read(int *x, int *y) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool active = now < s_sample.until;
    bool down = s_sample.down;
    if (active) { *x = s_sample.x; *y = s_sample.y; }
    portEXIT_CRITICAL(&s_lock);
    return active ? down : touch_read(x, y);
}
