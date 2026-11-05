#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "display_s3.h"
extern int mock_allocation_call, mock_allocation_fail, mock_held;
extern int mock_semaphore_call, mock_semaphore_fail, mock_task_calls;
extern bool mock_task_fail, mock_suppress_start;
extern atomic_bool mock_blocked, mock_credit_waiting, mock_done_waiting;
extern uint16_t mock_screen[448][368];
extern int mock_windows, mock_writes, mock_syncs;
extern int64_t mock_first_pixel;
void mock_hold(bool queue, bool done);
void mock_release(void);
void mock_release_queue(void);
void mock_wait_credit(void);
void mock_wait_done(void);
void mock_sleep(unsigned ms);
void mock_wait_blocked(void);
void mock_shutdown(void);
