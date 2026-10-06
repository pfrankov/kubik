#include "mic_capture.h"

static void count(volatile uint32_t *value) {
    if (*value < UINT32_MAX) (*value)++;
}

void mic_capture_init(mic_capture_t *c, void *context, mic_capture_toggle_fn rx,
                      mic_capture_toggle_fn adc, mic_capture_read_fn read,
                      bool rx_enabled, bool adc_enabled) {
    *c = (mic_capture_t){.context = context, .rx = rx, .adc = adc, .read = read,
                         .rx_enabled = rx_enabled, .adc_enabled = adc_enabled};
}

static bool turn_off(mic_capture_t *c) {
    bool ok = true;
    c->ready = false;
    if (c->adc_enabled && c->adc(c->context, false) == 0) c->adc_enabled = false;
    else if (c->adc_enabled) ok = false;
    if (!c->adc_enabled && c->rx_enabled && c->rx(c->context, false) == 0) c->rx_enabled = false;
    else if (c->rx_enabled) ok = false;
    return ok && !c->adc_enabled && !c->rx_enabled;
}

static bool turn_on(mic_capture_t *c) {
    if (c->adc_enabled && !c->ready && c->adc(c->context, false) == 0) c->adc_enabled = false;
    if (c->adc_enabled && !c->ready) return false;
    if (!c->rx_enabled) {
        if (c->rx(c->context, true) != 0) return false;
        c->rx_enabled = true;
    }
    if (!c->adc_enabled) {
        if (c->adc(c->context, true) != 0) {
            if (c->adc(c->context, false) == 0) c->adc_enabled = false;
            else c->adc_enabled = true;
            turn_off(c);
            return false;
        }
        c->adc_enabled = true;
    }
    c->ready = c->rx_enabled && c->adc_enabled;
    return c->ready;
}

bool mic_capture_sync(mic_capture_t *c, bool requested) {
    return requested ? turn_on(c) : turn_off(c);
}

bool mic_capture_read(mic_capture_t *c, bool permitted, void *buffer, size_t bytes, uint32_t timeout_ms) {
    if (!permitted || !c->ready || !c->rx_enabled || !c->adc_enabled) {
        count(&c->idle_reads);
        return false;
    }
    count(&c->reads);
    size_t read = 0;
    return c->read(c->context, buffer, bytes, &read, timeout_ms) == 0 && read == bytes;
}

void mic_capture_center(int16_t *buf, size_t samples, int32_t *dc) {
    for (size_t i = 0; i < samples; i++) {
        *dc += ((int32_t)buf[i] * 256 - *dc) >> 10;
        int32_t v = buf[i] - (*dc >> 8);
        buf[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
    }
}
