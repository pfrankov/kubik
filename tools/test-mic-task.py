#!/usr/bin/env python3
"""Exercise capture epochs and nonblocking RX shutdown using actual firmware functions."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, start, end):
    text = (ROOT / path).read_text()
    return text[text.index(start):text.index(end)]


task = '\n'.join(line for line in (ROOT / 'firmware/main/mic_capture_task.c').read_text().splitlines()
                 if not line.startswith('#include'))
stop = function('firmware/main/audio.c', 'void audio_mic_request_stop(', 'bool audio_mic_start_epoch(')
gate = function('firmware/main/audio.c', 'static void finish_capture_gate(', '// RX handlers')
queued = function('firmware/main/audio.c', 'static void deliver_queued(void *context, const int16_t *frame, const uint8_t *ima, uint32_t epoch) {', 'static void task_deliver(')
suspend = function('firmware/main/app_wake.c', 'void app_wake_suspend(', 'static void wake_frame(')
fixture = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mic_capture_task.h"
#include "ima_adpcm.h"
static void mic_capture_center(int16_t *buf,size_t n,int32_t *dc) {(void)buf;(void)n;(void)dc;}
#define RTC_NOINIT_ATTR
#define ESP_LOGE(tag,...) ((void)(tag))
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffu
static unsigned yields;
static void vTaskDelay(unsigned ms) { assert(ms == 1); yields++; }
static void ulTaskNotifyTake(int clear, unsigned wait) { (void)clear; (void)wait; assert(0); }
static jmp_buf done;
static unsigned reads, deliveries, epoch = 1;
static bool requested(void *ctx) { (void)ctx; return true; }
static bool sync_capture(void *ctx, bool open) { (void)ctx; assert(open); return true; }
static uint32_t capture_epoch(void *ctx) { (void)ctx; return epoch; }
static bool read_pcm(void *ctx, bool permitted, void *data, size_t bytes, uint32_t timeout) {
    (void)ctx; assert(permitted && bytes == 4 * 240 * sizeof(int16_t) && timeout == 20);
    if (++reads == 13) longjmp(done, 1);
    if (reads == 2 || reads == 8) ++epoch;
    int16_t *pcm = data;
    for (int i = 0; i < 960; ++i) pcm[i] = (int16_t)(100 * epoch + reads);
    return true;
}
static void deliver(void *ctx, int16_t *pcm, const uint8_t *reference, int32_t *dc, uint32_t frame_epoch) {
    (void)ctx; assert(!reference); ++deliveries;
    assert(*dc == 0 && frame_epoch == epoch && epoch == deliveries + 1);
    unsigned first_read = deliveries == 1 ? 3 : 9;
    for (unsigned i = 0; i < 960; ++i) assert(pcm[i] == (int16_t)(100 * epoch + first_read + i / 240));
    *dc = 99;
}
static bool s_gate = true, s_mic_closing, s_pending, s_automatic;
static uint32_t s_capture_epoch = 7;
bool audio_mic_request_stop_epoch(uint32_t epoch);
static float s_mic_level = 1;
static int s_audio_mux, s_mic_task = 1, notifications, suspends, events, s_auto_turn = 42;
// No mic mutex is available in this fixture: RX owns route while mic owns its lock.
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef void (*mic_frame_fn)(const int16_t *, int, const uint8_t *);
static int s_mic_read_lock, mutex_depth;
static bool s_wake_capture, s_duplex;
static mic_frame_fn s_mic_fn;
static void audio_sfx_quiet(void) {}
typedef void (*mic_delivery_frame_fn)(void *,const int16_t *,const uint8_t *,uint32_t);
static void deliver_queued(void *,const int16_t *,const uint8_t *,uint32_t);
#define MIC_FRAME_SAMPLES 960
static uint32_t audio_capture_epoch(void) {return s_capture_epoch;}
static unsigned delivered_frames;
static uint32_t queued_epoch;
static bool mic_delivery_start(mic_delivery_frame_fn fn,void *ctx,bool duplex) {assert(fn==deliver_queued&&!ctx&&!duplex);return true;}
static void mic_delivery_stop(void) {
    assert(mutex_depth==0); // Worker must be able to finish outside the producer lock.
    if(queued_epoch) {int16_t pcm=1;deliver_queued(NULL,&pcm,NULL,queued_epoch);queued_epoch=0;}
}
static void vTaskPrioritySet(int task,int priority) {assert(task==1&&(priority==6||priority==9));}
static void audio_kick(void) {}
static void xSemaphoreTake(int lock, unsigned wait) { (void)lock; (void)wait; ++mutex_depth; }
static void xSemaphoreGive(int lock) { (void)lock; --mutex_depth; }
static void wait_capture(bool open) { (void)open; }
static bool capture_stopped(void) { return !s_gate; }
static bool capture_done(bool open) { return s_gate == open; }
static void frame(const int16_t *pcm, int samples, const uint8_t *ima) { assert(pcm && samples==960 && !ima); delivered_frames++; }
static void xTaskNotifyGive(int task) { assert(task == 1); notifications++; }
static void suspend_model(void) { suspends++; }
#define EV_VOICE_END 2
static void app_post(int type, int turn, int reason) {
    assert(type == EV_VOICE_END && turn == 42 && reason == 2); events++;
}
'''
tests = r'''
static bool duplex_capture(void *ctx) {(void)ctx;return true;}
static void reference_packing(void) {
    mic_task_config_t cfg={.duplex=duplex_capture,.slots=4,.voice_slot=3,.chunk_samples=240};
    ima_state_t actual={0},oracle={0};int32_t dc=0;
    int16_t raw[960],voice[960],near[960],far[960],decoded[960],expected[960];
    uint8_t packed[483],whole[483];
    for(int frame=0;frame<3;frame++) {
        if(frame==2) actual=oracle=(ima_state_t){0}; // A new capture epoch resets both predictors.
        for(int chunk=0;chunk<4;chunk++) {
            for(int i=0;i<240;i++) {
                int at=chunk*240+i;
                near[at]=(int16_t)(1000+at+frame*200);
                far[at]=(int16_t)((at*71+frame*937)%6000-3000);
                raw[4*i]=-123;raw[4*i+1]=far[at];raw[4*i+2]=456;raw[4*i+3]=near[at];
            }
            assert(copy_channels(&cfg,raw,voice,packed,chunk*240,&actual,&dc));
        }
        ima_encode(&oracle,far,960,whole);
        assert(!memcmp(packed,whole,483)&&!memcmp(voice,near,sizeof voice));
        ima_state_t a,b;ima_read_header(packed,&a);ima_read_header(whole,&b);
        ima_decode(&a,packed+3,480,decoded);ima_decode(&b,whole+3,480,expected);
        assert(!memcmp(decoded,expected,sizeof decoded));
        assert(actual.pred==oracle.pred&&actual.index==oracle.index);
    }
}
int main(void) {
    reference_packing();
    mic_task_config_t config = { .requested = requested, .sync = sync_capture, .read = read_pcm,
        .deliver = deliver, .epoch = capture_epoch, .slots = 4, .chunk_samples = 240,
        .frame_samples = 960, .voice_slot = 1, .timeout_ms = 20 };
    if (!setjmp(done)) mic_capture_task(&config);
    assert(deliveries == 2 && yields == 2); // no mixed/partial pre-transition audio reached either callback
    s_automatic = true; app_wake_suspend();
    assert(!s_gate && s_mic_closing && s_capture_epoch == 8 && !s_mic_level);
    assert(s_pending && events == 1 && suspends == 1 && notifications == 1);
    s_automatic = false; app_wake_suspend();
    assert(events == 1 && suspends == 2 && notifications == 1);
    uint32_t expected = s_capture_epoch;
    audio_mic_request_stop(); // crossed incoming speech invalidates the pending open
    assert(!capture_gate(true, frame, false, &expected));
    assert(!s_gate && s_mic_fn == NULL && mutex_depth == 0);
    expected = s_capture_epoch;
    assert(capture_gate(true, frame, false, &expected));
    assert(s_gate && s_mic_fn == frame && s_capture_epoch == expected + 1 && !s_wake_capture);
    assert(!audio_mic_request_stop_epoch(expected));
    assert(s_gate && s_capture_epoch == expected + 1);
    queued_epoch=s_capture_epoch;
    assert(audio_mic_request_stop_epoch(expected + 1));
    assert(!s_gate && s_capture_epoch == expected + 2);
    assert(capture_gate(false, NULL, false, NULL));
    assert(s_mic_fn == NULL && mutex_depth == 0 && delivered_frames==0); // Async terminal event discarded old queued PCM.
    assert(capture_gate(true, frame, false, NULL));
    queued_epoch=s_capture_epoch;
    assert(capture_gate(false, NULL, false, NULL));
    assert(delivered_frames==1 && !s_mic_fn); // User close drained accepted PCM before changing the epoch.
    puts("mic task: epoch transitions discard partial frames; RX suspension never waits for mic callback");
}
'''
with tempfile.TemporaryDirectory(prefix='c6-mic-task-') as directory:
    source = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    source.write_text(fixture + task + queued + gate + stop + suspend + tests)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'firmware/main'), str(source), str(ROOT / "firmware/main/ima_adpcm.c"), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
