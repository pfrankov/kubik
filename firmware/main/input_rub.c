#include "input_rub.h"

#include <math.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static rub_track_t s_track;
static rub_input_t s_input;

void input_rub_sample(bool down, int x, int y) {
    portENTER_CRITICAL(&s_lock);
    rub_touch(&s_track, &s_input, down, x, y);
    portEXIT_CRITICAL(&s_lock);
}

void input_rub(rub_input_t *input) {
    portENTER_CRITICAL(&s_lock);
    *input = s_input;
    s_input.path = 0;
    s_input.turns = 0;
    portEXIT_CRITICAL(&s_lock);
}

void input_rub_script(float t, bool cloud, bool down) {
    const float turn = 6.2831853f;
    input_rub_sample(down, (int)(240 + 60 * sinf(turn * 3.5f * t)), (int)((cloud ? 255 : 370) + 15 * sinf(turn * 1.7f * t)));
}
