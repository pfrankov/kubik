#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../main/mic_capture.h"

typedef struct {
    char events[32];
    size_t event_count;
    int reads;
    bool rx_on, adc_on, fail_adc_on, fail_adc_off;
    int read_error;
    bool short_read;
} mock_t;

static int toggle(mock_t *m, char name, bool enabled, bool *state, bool fail) {
    m->events[m->event_count++] = name;
    m->events[m->event_count++] = enabled ? '+' : '-';
    m->events[m->event_count] = 0;
    if (fail) return -1;
    *state = enabled;
    return 0;
}
static int rx(void *context, bool enabled) {
    mock_t *m = context;
    return toggle(m, 'R', enabled, &m->rx_on, false);
}
static int adc(void *context, bool enabled) {
    mock_t *m = context;
    bool fail = enabled ? m->fail_adc_on : m->fail_adc_off;
    if (enabled) m->fail_adc_on = false;
    else m->fail_adc_off = false;
    return toggle(m, 'A', enabled, &m->adc_on, fail);
}
static int read_frame(void *context, void *buffer, size_t bytes, size_t *read, uint32_t timeout_ms) {
    mock_t *m = context;
    (void)buffer;
    (void)timeout_ms;
    m->reads++;
    *read = m->short_read ? bytes - 1 : bytes;
    return m->read_error;
}
static void init(mic_capture_t *capture, mock_t *mock) {
    memset(mock, 0, sizeof *mock);
    mic_capture_init(capture, mock, rx, adc, read_frame, false, false);
}

static void ptt_only_reads(void) {
    mic_capture_t capture;
    mock_t mock;
    uint8_t frame[32];
    init(&capture, &mock);
    assert(!mic_capture_read(&capture, false, frame, sizeof frame, 20));
    assert(!mic_capture_read(&capture, true, frame, sizeof frame, 20));
    assert(mock.reads == 0 && capture.reads == 0 && capture.idle_reads == 2);
    assert(mic_capture_sync(&capture, true));
    assert(strcmp(mock.events, "R+A+") == 0);
    assert(mic_capture_read(&capture, true, frame, sizeof frame, 20));
    assert(mock.reads == 1 && capture.reads == 1 && capture.idle_reads == 2);
    assert(mic_capture_sync(&capture, false));
    assert(strcmp(mock.events, "R+A+A-R-") == 0);
    assert(!mock.rx_on && !mock.adc_on && !capture.ready);
    assert(!mic_capture_read(&capture, true, frame, sizeof frame, 20));
    assert(mock.reads == 1 && capture.reads == 1 && capture.idle_reads == 3);
}

static void failed_start_rolls_back_rx(void) {
    mic_capture_t capture;
    mock_t mock;
    init(&capture, &mock);
    mock.fail_adc_on = true;
    assert(!mic_capture_sync(&capture, true));
    assert(strcmp(mock.events, "R+A+A-R-") == 0);
    assert(!mock.rx_on && !mock.adc_on && !capture.ready);
}

static void failed_stop_keeps_rx_until_adc_is_off(void) {
    mic_capture_t capture;
    mock_t mock;
    init(&capture, &mock);
    assert(mic_capture_sync(&capture, true));
    mock.fail_adc_off = true;
    assert(!mic_capture_sync(&capture, false));
    assert(strcmp(mock.events, "R+A+A-") == 0);
    assert(mock.rx_on && mock.adc_on && !capture.ready);
    assert(mic_capture_sync(&capture, false));
    assert(strcmp(mock.events, "R+A+A-A-R-") == 0);
    assert(!mock.rx_on && !mock.adc_on);
}

static void incomplete_and_failed_reads_are_counted(void) {
    mic_capture_t capture;
    mock_t mock;
    uint8_t frame[32];
    init(&capture, &mock);
    assert(mic_capture_sync(&capture, true));
    mock.read_error = -1;
    assert(!mic_capture_read(&capture, true, frame, sizeof frame, 20));
    mock.read_error = 0;
    mock.short_read = true;
    assert(!mic_capture_read(&capture, true, frame, sizeof frame, 20));
    assert(mock.reads == 2 && capture.reads == 2);
    assert(mic_capture_sync(&capture, false));
}

int main(void) {
    ptt_only_reads();
    failed_start_rolls_back_rx();
    failed_stop_keeps_rx_until_adc_is_off();
    incomplete_and_failed_reads_are_counted();
    puts("mic_capture: idle reads blocked; RX/ADC start and stop order, rollback, read counters passed");
    return 0;
}
