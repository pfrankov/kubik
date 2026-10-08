#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include "input_probe.h"

static int64_t clock_us;
static unsigned reads;
int64_t esp_timer_get_time(void) { return clock_us; }
bool touch_read(int *x, int *y) { reads++; *x = 31; *y = 42; return true; }

static void expiry_and_invalid_samples(void) {
    int x = -1, y = -1;
    assert(input_probe_read(&x, &y) && x == 31 && y == 42 && reads == 1);
    assert(input_probe_sample(true, 340, 255));
    assert(input_probe_read(&x, &y) && x == 340 && y == 255 && reads == 1);
    assert(!input_probe_sample(false, -1, 0));
    assert(!input_probe_sample(true, 480, 0));
    assert(!input_probe_sample(true, 0, 480));
    clock_us = 349999;
    assert(input_probe_read(&x, &y) && x == 340 && reads == 1);
    clock_us++;
    assert(input_probe_read(&x, &y) && x == 31 && reads == 2);
}
static void release_and_refresh(void) {
    int x = -1, y = -1;
    assert(input_probe_sample(false, 340, 255));
    assert(!input_probe_read(&x, &y) && reads == 2);
    clock_us += 100000;
    assert(input_probe_sample(true, 240, 355));
    clock_us += 300000;
    assert(input_probe_read(&x, &y) && x == 240 && y == 355 && reads == 2);
    clock_us += 50000;
    assert(input_probe_read(&x, &y) && x == 31 && reads == 3);
}
static void *writer(void *unused) {
    (void)unused;
    for (int n = 0; n < 50000; n++) assert(input_probe_sample(true, n % 480, 479 - n % 480));
    return NULL;
}
static void concurrent_samples_are_coherent(void) {
    pthread_t thread;
    assert(input_probe_sample(true, 0, 479));
    assert(pthread_create(&thread, NULL, writer, NULL) == 0);
    for (int n = 0; n < 50000; n++) {
        int x, y;
        assert(input_probe_read(&x, &y) && x + y == 479);
    }
    assert(pthread_join(thread, NULL) == 0);
}
int main(void) {
    expiry_and_invalid_samples();
    release_and_refresh();
    concurrent_samples_are_coherent();
    puts("touch probe: exact expiry, refreshed samples, release, invalid coordinates and real-controller restoration passed");
}
