#!/usr/bin/env python3
"""Completed S3 DMA events and C6 synchronous duration keep distinct ownership."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'firmware/main/present.c').read_text()
helpers = source[source.index('static void account_burst('):source.index('bool present_frame(')]
fixture = r'''
#include <assert.h>
#include <stdint.h>
typedef struct { int64_t sum_us, max_us; uint32_t count; } display_s3_tx_bursts_t;
typedef struct {
    int64_t burst_sum, burst_max, bursts, burst_start, shown_at, gap_min, gap_max;
} frame_out_t;
static display_s3_tx_bursts_t completed;
static int clock_reads;
static int64_t clock_now=200;
static int64_t esp_timer_get_time(void) { ++clock_reads; return clock_now; }
static display_s3_tx_bursts_t display_s3_tx_take_bursts(void) {
    display_s3_tx_bursts_t result = completed;
    completed = (display_s3_tx_bursts_t){0}; return result;
}
/* ACTUAL_HELPERS */
int main(void) {
    frame_out_t frame = {.burst_start=100};
#if CONFIG_IDF_TARGET_ESP32S3
    completed = (display_s3_tx_bursts_t){.sum_us=18000,.max_us=12000,.count=2};
    account_presentation(&frame);
    assert(frame.burst_sum==18000 && frame.burst_max==12000 && frame.bursts==2);
    assert(clock_reads==0);  // START acknowledgement is not a completed wire duration.
    frame.burst_start=0;  // A completion can arrive while the current frame changes nothing.
    completed = (display_s3_tx_bursts_t){.sum_us=22000,.max_us=22000,.count=1};
    account_presentation(&frame);
    account_presentation(&frame);
    assert(frame.burst_sum==40000 && frame.burst_max==22000 && frame.bursts==3);
#else
    account_presentation(&frame);
    assert(frame.burst_sum==100 && frame.burst_max==100 && frame.bursts==1 && clock_reads==1);
    frame.burst_start=0; account_presentation(&frame);
    assert(frame.bursts==1 && clock_reads==1);
#endif
    assert(frame.shown_at==0);
    frame.burst_start=1000; clock_now=1010; account_presentation(&frame);
    assert(frame.gap_max==0); // No fictitious spacing across an unchanged frame.
    frame.burst_start=34500; clock_now=34510; account_presentation(&frame);
    assert(frame.gap_min==33500 && frame.gap_max==33500);
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-bursts-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(fixture.replace('/* ACTUAL_HELPERS */', helpers))
    for target in (0, 1):
        binary = Path(directory) / f'test-{target}'
        subprocess.run(['cc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                        f'-DCONFIG_IDF_TARGET_ESP32S3={target}', str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)
print('PASS completed S3 burst aggregation, START gaps, and unchanged C6 duration')
