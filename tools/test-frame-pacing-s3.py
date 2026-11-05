#!/usr/bin/env python3
"""Exercise production S3 pacing against a measured panel clock and kernel waits."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MOCK = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
typedef int esp_err_t;
typedef unsigned TickType_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_TIMEOUT 2
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portTICK_PERIOD_MS 1
static jmp_buf failure;
static int fatal;
#define ESP_ERROR_CHECK(result) do {int error=(result);if(error){fatal=error;longjmp(failure,1);}} while(0)
#include "display_te_s3.h"
static int64_t now,epoch,accepted;
static uint32_t period;
static int mode,waits,reads,delays,takes,early_once;
static uint32_t notifications,notify_during_wait;
static int64_t delay_total;
static int64_t esp_timer_get_time(void) {return now;}
bool display_te_clock_snapshot(display_te_clock_t *out) {
    reads++;
    int64_t latest=mode==4 ? epoch : epoch+((now-epoch)/period)*period;
    *out=(display_te_clock_t){accepted,latest,(mode==1 || mode==2 || mode==3) ? 0 : period};
    return out->accepted_edge_us>0 && out->period_us>0;
}
esp_err_t display_te_wait(void) {
    waits++;
    if(mode==2 || mode==4){now+=40000;return ESP_ERR_TIMEOUT;}
    now=epoch+((now-epoch)/period+1)*period;accepted=now;
    if(mode==1 && waits==2)mode=0;
    return ESP_OK;
}
static void vTaskDelay(TickType_t ticks) {
    assert(ticks>0 && ticks<=100);delays++;
    int64_t duration=(int64_t)ticks*1000-early_once;early_once=0;
    assert(duration>0);now+=duration;delay_total+=duration;
    notifications+=notify_during_wait;
}
static uint32_t ulTaskNotifyTake(int clear,TickType_t ticks) {
    assert(clear==pdTRUE && (ticks==0 || ticks==250));takes++;
    uint32_t pending=notifications;notifications=0;
    if(!pending)now+=(int64_t)ticks*1000;
    return pending;
}
'''
TEST = r'''
#include "frame_pacing_s3.c"
static void reset(uint32_t measured) {
    epoch=accepted=1000000;now=epoch+8000;period=measured;
    mode=waits=reads=delays=takes=early_once=fatal=0;
    notifications=notify_during_wait=0;delay_total=0;
    s_accepted_edge=s_next_slot=s_expected_edge=0;
}
int main(void) {
    if(setjmp(failure))assert(false && "unexpected clock error");
    // Preparation starts one measured period after presentation.
    // A six-ms preparation then reaches the second following physical edge.
    reset(16667);
    for(int frame=0;frame<100;frame++) {
        int64_t edge=accepted;int old_delays=delays,old_takes=takes;
        assert(!frame_pacing_s3_wait(true));
        assert(now>=edge+period && now<edge+period+2000);
        assert(delays-old_delays<=2 && takes-old_takes==1 && !waits);
        now+=6000;
        int64_t next=epoch+((now-epoch)/period+1)*period;
        assert(next==edge+2*period);
        accepted=next;now=accepted+12000;
    }
    // Actual period changes are used directly, never a fixed 16.667-ms rate.
    period=17321;epoch=accepted;now=accepted+12000;
    assert(!frame_pacing_s3_wait(true) && now>=accepted+period && now<accepted+period+2000);
    // A newly accepted third TE is late even though reanchoring creates a
    // future preparation slot. The following on-time edge is not counted twice.
    reset(16750);assert(!frame_pacing_s3_wait(true));
    accepted=epoch+3*period;now=accepted+12000;
    assert(frame_pacing_s3_wait(true) && now>=accepted+period);
    accepted+=2*period;now=accepted+12000;
    assert(!frame_pacing_s3_wait(true));
    // Repeated small changes in the measured period do not turn an on-time
    // second edge into a skipped slot; an actual third edge still does.
    reset(16667);assert(!frame_pacing_s3_wait(true));
    period=17321;accepted=epoch+2*period;now=accepted+12000;
    assert(!frame_pacing_s3_wait(true));
    epoch=accepted;period=16800;accepted=epoch+3*period;now=accepted+12000;
    assert(frame_pacing_s3_wait(true));
    // No-damage frames retain their accepted edge and advance their own slot.
    reset(16750);
    int64_t expected=accepted+period;
    for(int frame=0;frame<40;frame++) {
        int64_t before=now;int old_delays=delays;
        assert(!frame_pacing_s3_wait(true));
        assert(now>=expected && now<expected+2000 && now-before<=2*period+2000);
        assert(delays-old_delays<=2 && !waits);
        expected+=2*period;now+=5000;
    }
    // An actual presentation after virtual no-damage slots is compared with
    // their latest deadline, not the long-unchanged physical presentation.
    accepted=s_expected_edge;now=accepted+12000;
    assert(!frame_pacing_s3_wait(true));
    assert(!frame_pacing_s3_wait(true));now+=5000;
    accepted=s_expected_edge+period;now=accepted+12000;
    assert(frame_pacing_s3_wait(true));
    // A notification storm cannot repeatedly wake an active pacing loop.
    reset(16750);notifications=100000;notify_during_wait=100000;
    assert(!frame_pacing_s3_wait(true) && takes==1 && delays<=2 && !notifications);
    // Fractional kernel tick phase gets at most one extra blocking tick.
    reset(16750);early_once=1000;
    assert(!frame_pacing_s3_wait(true) && now>=accepted+period && delays==2);
    // A late CPU frame yields once and skips expired no-damage slots.
    reset(16750);now=accepted+2*period+500;
    assert(frame_pacing_s3_wait(true) && delays==1 && delay_total==1000 && s_next_slot>now);
    now+=1000;assert(!frame_pacing_s3_wait(true) && s_next_slot>now);
    reset(16750);now=accepted+2*period+500;
    assert(frame_pacing_s3_wait(true));
    accepted=epoch+3*period;now=accepted+12000;
    assert(!frame_pacing_s3_wait(true));
    // Deliberate yielding must not turn a still-open deadline into an overload.
    reset(16750);now=accepted+2*period-1;
    assert(!frame_pacing_s3_wait(true) && now>accepted+2*period);
    // Dark polling remains immediately notification-interruptible and resets phase.
    reset(16750);assert(!frame_pacing_s3_wait(true));int old_reads=reads;
    notifications=1;int64_t before=now;
    assert(!frame_pacing_s3_wait(false) && now==before && reads==old_reads &&
           !s_next_slot && !s_expected_edge);
    assert(!frame_pacing_s3_wait(false) && now==before+250000 && reads==old_reads);
    accepted=epoch=now;now+=12000;
    assert(!frame_pacing_s3_wait(true) && now>=accepted+period);
    // Both 5-ms and 12-ms preparations present at E+2T after the staging path.
    for(int duration=5000;duration<=12000;duration+=7000) {
        reset(16750);now=epoch+8400;
        assert(!frame_pacing_s3_wait(true));now+=duration;
        assert(frame_pacing_s3_sync()==ESP_OK);
        assert(accepted==epoch+2*period && waits==1);
        now=accepted+12000;assert(!frame_pacing_s3_wait(true));
    }
    reset(16750);now=epoch+8400;
    assert(!frame_pacing_s3_wait(true));now+=19000;
    assert(frame_pacing_s3_sync()==ESP_OK && accepted==epoch+3*period);
    now=accepted+12000;assert(frame_pacing_s3_wait(true));
    // The synchronizer still rejects a fresh early edge if preparation is
    // already complete before E+T, with a strict two-fresh-edge bound.
    reset(16750);s_expected_edge=epoch+2*period;
    assert(frame_pacing_s3_sync()==ESP_OK && accepted==epoch+2*period && waits==2);
    // Initial and post-dark presentations accept their first fresh edge.
    reset(16750);assert(frame_pacing_s3_sync()==ESP_OK && waits==1);
    assert(!frame_pacing_s3_wait(true));notifications=1;
    assert(!frame_pacing_s3_wait(false));waits=0;
    assert(frame_pacing_s3_sync()==ESP_OK && waits==1);
    // Sync uses measured drift and the virtual no-damage presentation target.
    reset(16750);assert(!frame_pacing_s3_wait(true));now+=3000;
    assert(!frame_pacing_s3_wait(true));now+=5000;
    int64_t target=s_expected_edge;
    assert(frame_pacing_s3_sync()==ESP_OK && accepted==target);
    reset(16667);now=epoch+8400;assert(!frame_pacing_s3_wait(true));
    period=17321;now+=5000;
    assert(frame_pacing_s3_sync()==ESP_OK && accepted==epoch+2*period);
    // Missing IRQ and unqualified clock errors propagate; early edges are
    // bounded to two waits and cannot authorize an unsynchronized transfer.
    reset(16750);mode=2;
    assert(frame_pacing_s3_sync()==ESP_ERR_TIMEOUT && waits==1);
    reset(16750);assert(!frame_pacing_s3_wait(true));mode=3;
    assert(frame_pacing_s3_sync()==ESP_ERR_INVALID_STATE && waits==1);
    reset(16750);s_expected_edge=epoch+10*period;
    assert(frame_pacing_s3_sync()==ESP_ERR_TIMEOUT && waits==2);
    // Startup qualification uses at most two bounded fresh-edge waits.
    reset(16750);mode=1;accepted=0;
    assert(!frame_pacing_s3_wait(true) && waits==2 && now>=accepted+period);
    reset(16750);mode=2;accepted=0;
    if(!setjmp(failure)){frame_pacing_s3_wait(true);assert(false);}
    assert(fatal==ESP_ERR_TIMEOUT && waits==1 && !delays && !takes);
    reset(16750);mode=3;accepted=0;
    if(!setjmp(failure)){frame_pacing_s3_wait(true);assert(false);}
    assert(fatal==ESP_ERR_INVALID_STATE && waits==2 && !delays && !takes);
    reset(16750);mode=4;now=epoch+100000;
    if(!setjmp(failure)){frame_pacing_s3_wait(true);assert(false);}
    assert(fatal==ESP_ERR_TIMEOUT && waits==1 && !delays);
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-s3-pacing-') as temporary:
    directory = Path(temporary)
    for name in ('esp_err.h', 'esp_timer.h', 'freertos/FreeRTOS.h', 'freertos/task.h'):
        header = directory / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "mock.h"\n')
    types, clock_mock = MOCK.split('#include "display_te_s3.h"', 1)
    (directory / 'mock.h').write_text(types)
    for name in ('frame_pacing_s3.c', 'frame_pacing_s3.h', 'display_te_s3.h'):
        (directory / name).write_text((ROOT / 'firmware/main' / name).read_text())
    source, executable = directory / 'test.c', directory / 'test'
    source.write_text('#include "mock.h"\n#include "display_te_s3.h"\n' + clock_mock + TEST)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(directory), str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
print('S3 pacing: measured TE cadence/drift, no-damage slots, overload, bounded notification work, '
      'target-edge synchronization, accepted-edge misses, dark/wake and missing-clock failures passed')
