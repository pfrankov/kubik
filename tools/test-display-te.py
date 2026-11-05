#!/usr/bin/env python3
"""Run the production S3 TE synchronizer against deterministic GPIO/RTOS mocks."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
pin = re.search(r'^#define PIN_LCD_TE (\d+)$', (ROOT / 'firmware/main/board.h').read_text(), re.M)
assert pin and int(pin[1]) == 13, 'The 1.8-inch schematic connects LCD_TE to GPIO13'

MOCK = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_TIMEOUT 3
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define GPIO_MODE_INPUT 1
#define GPIO_INTR_POSEDGE 2
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
typedef int BaseType_t;
typedef int *SemaphoreHandle_t;
typedef struct {uint64_t pin_bit_mask; int mode, intr_type;} gpio_config_t;
static int mutex_depth;
#define portENTER_CRITICAL(lock) do {(void)(lock); assert(!mutex_depth++);} while (0)
#define portEXIT_CRITICAL(lock) do {(void)(lock); assert(mutex_depth-- == 1);} while (0)
#define portENTER_CRITICAL_ISR(lock) portENTER_CRITICAL(lock)
#define portEXIT_CRITICAL_ISR(lock) portEXIT_CRITICAL(lock)
static int token, creates, deletes, clears, blocking_takes, spi_drains, bursts, yields;
static int gpio_failure, install_result, handler_failure, allocation_failure, inject;
static int64_t clock_us;
static void (*handler)(void *);
static void *handler_arg;
static int64_t esp_timer_get_time(void) { return clock_us; }
#define portYIELD_FROM_ISR() (++yields)
static SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    creates++; token = 0; return allocation_failure ? NULL : &token;
}
static void vSemaphoreDelete(SemaphoreHandle_t semaphore) { assert(semaphore == &token); deletes++; }
static void xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore, BaseType_t *woken) {
    assert(semaphore == &token); token = 1; *woken = pdTRUE;
}
static int xSemaphoreTake(SemaphoreHandle_t semaphore, int ticks) {
    assert(semaphore == &token && !mutex_depth);
    if (!ticks) clears++;
    else {
        assert(ticks == 40 && spi_drains > 0 && clears > 0);
        blocking_takes++;
        if (inject) { clock_us += 16667; handler(handler_arg); }
        else clock_us += ticks * 1000;
    }
    int previous = token; token = 0; return previous;
}
static int gpio_config(const gpio_config_t *cfg) {
    assert(cfg->pin_bit_mask == (1ULL << 13));
    assert(cfg->mode == GPIO_MODE_INPUT && cfg->intr_type == GPIO_INTR_POSEDGE);
    return gpio_failure;
}
static int gpio_install_isr_service(int flags) { assert(flags == 0); return install_result; }
static int gpio_isr_handler_add(int pin, void (*callback)(void *), void *arg) {
    assert(pin == 13 && callback); handler = callback; handler_arg = arg; return handler_failure;
}
static void board_display_wait_idle(void) { spi_drains++; }
'''

TEST = r'''
#include "display_te_s3.c"
static esp_err_t present(void) {
    esp_err_t result = display_te_wait();
    if (result == ESP_OK) bursts++; // The production adapter checks this result before its SPI burst.
    return result;
}
int main(void) {
    assert(display_te_wait() == ESP_ERR_INVALID_STATE && !spi_drains);
    allocation_failure = 1;
    assert(display_te_init() == ESP_ERR_NO_MEM && !s_te_edge && !deletes);
    allocation_failure = 0; gpio_failure = 9;
    assert(display_te_init() == 9 && !s_te_edge && deletes == 1);
    gpio_failure = 0; install_result = 8;
    assert(display_te_init() == 8 && !s_te_edge && deletes == 2);
    install_result = ESP_ERR_INVALID_STATE; handler_failure = 7;
    assert(display_te_init() == 7 && !s_te_edge && deletes == 3);
    handler_failure = 0;
    assert(display_te_init() == ESP_OK && s_te_edge);
    int created = creates;
    assert(display_te_init() == ESP_ERR_INVALID_STATE && creates == created);
    // A queued old edge cannot authorize a pixel burst without a new edge.
    clock_us = 100; handler(NULL);
    assert(token && display_te_last_edge_us() == 0);
    assert(present() == ESP_ERR_TIMEOUT && bursts == 0 && blocking_takes == 1);
    assert(clock_us == 40100 && display_te_last_edge_us() == 0);
    // A newly delivered vertical edge is consumed once and its ISR time is retained.
    inject = 1;
    assert(present() == ESP_OK && bursts == 1 && display_te_last_edge_us() == 56767);
    assert(!token && !mutex_depth && yields == 2);
    // A late extra edge never changes the edge accepted by the completed wait.
    clock_us += 10; handler(NULL);
    assert(display_te_last_edge_us() == 56767);
    inject = 0;
    assert(present() == ESP_ERR_TIMEOUT && bursts == 1 && display_te_last_edge_us() == 56767);
    assert(blocking_takes == 3 && clears == 3 && spi_drains == 3);
    // Both a fresh service and a pre-existing non-IRAM service are supported.
    vSemaphoreDelete(s_te_edge); s_te_edge = NULL; install_result = ESP_OK;
    assert(display_te_init() == ESP_OK);
    inject = 1;
    assert(present() == ESP_OK && bursts == 2);
    display_te_clock_t clock;
    assert(!display_te_clock_snapshot(NULL));
    assert(!display_te_clock_snapshot(&clock)); // A gap is not a valid measured period.
    assert(present() == ESP_OK && bursts == 3);
    assert(display_te_clock_snapshot(&clock) && clock.period_us == 16667);
    assert(clock.accepted_edge_us == clock.latest_edge_us);
    int64_t accepted = clock.accepted_edge_us;
    clock_us += 17234; handler(NULL);
    assert(display_te_clock_snapshot(&clock) && clock.period_us == 17234);
    assert(clock.accepted_edge_us == accepted && clock.latest_edge_us == clock_us);
    clock_us += 9999; handler(NULL);assert(!display_te_clock_snapshot(&clock) && !clock.period_us);
    clock_us += 25001; handler(NULL);assert(!display_te_clock_snapshot(&clock) && !clock.period_us);
    clock_us += 10000; handler(NULL);assert(display_te_clock_snapshot(&clock) && clock.period_us == 10000);
    clock_us += 25000; handler(NULL);assert(display_te_clock_snapshot(&clock) && clock.period_us == 25000);
}
'''

with tempfile.TemporaryDirectory(prefix='kubik-display-te-') as temporary:
    directory = Path(temporary)
    for name in ('driver/gpio.h', 'freertos/FreeRTOS.h', 'freertos/semphr.h', 'esp_err.h', 'esp_timer.h'):
        file = directory / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text('#include "mock.h"\n')
    (directory / 'mock.h').write_text(MOCK)
    (directory / 'board.h').write_text('#include "mock.h"\n' + pin[0] + '\n')
    for name in ('display_te_s3.c', 'display_te_s3.h'):
        (directory / name).write_text((ROOT / 'firmware/main' / name).read_text())
    source, executable = directory / 'test.c', directory / 'test'
    source.write_text(TEST)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(directory), str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('S3 TE: GPIO13, fresh edge after drained SPI, stale rejection, 40 ms bound, ISR timestamp, failure cleanup and no unsynchronized burst passed')
