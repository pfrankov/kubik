#!/usr/bin/env python3
"""Actual ring reader/restart exclusion; a blocked A decode cannot advance B's reset cursors."""
import os
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
audio = (ROOT / "firmware/main/audio.c").read_text()
state = audio[audio.index("static uint8_t *s_ring"):audio.index("static volatile bool s_speech_audible")]
doze = audio[audio.index("static bool may_doze"):audio.index("static void doze_while_quiet")]
reader = audio[audio.index("static bool decode_speech_frame"):audio.index("static void mix_speaker_frame")]
mock = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ima_adpcm.h"
#include "audio_output_tail.h"
#define RING_BYTES @RING_BYTES@
#define RING_BLOCKS 3
#define RING_BLOCK_BYTES (RING_BYTES/RING_BLOCKS)
#define PREBUF_BYTES @PREBUF_BYTES@
#define REBUF_BYTES @REBUF_BYTES@
#define FRAME_BYTES 240
#define SPK_FRAME AUDIO_SPEAKER_FRAMES
#define RING_BYTES_PER_MS 12
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_RTCRAM 4
#define portMAX_DELAY 1
#define pdTRUE 1
#define ESP_LOGW(...) ((void)0)
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
typedef pthread_mutex_t *SemaphoreHandle_t;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
static pthread_mutex_t chunk = PTHREAD_MUTEX_INITIALIZER, gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool decoding, release_decode, restart_started, restart_done;
static unsigned allocation_limit = 3, allocations, kicks;
static bool probe_kick;
static int xSemaphoreTake(SemaphoreHandle_t m, int timeout) { return (timeout ? pthread_mutex_lock(m) : pthread_mutex_trylock(m)) == 0; }
static void xSemaphoreGive(SemaphoreHandle_t m) { assert(!pthread_mutex_unlock(m)); }
static void *heap_caps_malloc(size_t bytes, int flags) {
    assert(bytes==3600 && flags==((allocations%3 < 2 ? MALLOC_CAP_RTCRAM : MALLOC_CAP_INTERNAL)|MALLOC_CAP_8BIT));
    return allocations++ < allocation_limit ? malloc(bytes) : NULL;
}
static void heap_caps_free(void *p) { free(p); }
static void audio_sfx_quiet(void) {}
static int64_t now;
static int64_t esp_timer_get_time(void) { return now; }

int ima_read_header(const uint8_t *p, ima_state_t *st) { (void)p; *st = (ima_state_t){0}; return true; }
void ima_decode(ima_state_t *st, const uint8_t *p, size_t bytes, int16_t *out) {
    (void)st; (void)p;
    pthread_mutex_lock(&gate); decoding = true; pthread_cond_broadcast(&condition);
    while (!release_decode) pthread_cond_wait(&condition, &gate);
    pthread_mutex_unlock(&gate);
    for (size_t i = 0; i < bytes * 2; i++) out[i] = 123;
}
'''
power_mock = r'''
void audio_kick(void) {
    assert(!pthread_mutex_trylock(&chunk)); pthread_mutex_unlock(&chunk);
    if (probe_kick) assert(s_active); // speaker must observe published begin state at its wake
    kicks++;
}
static bool s_doze, s_gate, s_mic_transition, effect, self_audible;
static struct { bool rx_enabled, adc_enabled; } s_capture;
static portMUX_TYPE s_audio_mux;
bool audio_sfx_playing(void) { return effect; }
bool audio_self_audible(void) { return self_audible; }
'''
test = r'''
static void *read_a(void *arg) {
    (void)arg; int16_t samples[480]; int32_t mix[480] = {0};
    assert(read_speech_frame(samples, mix)); assert(mix[0] == 123); return NULL;
}
static void *begin_b(void *arg) {
    (void)arg;
    pthread_mutex_lock(&gate); restart_started = true; pthread_cond_broadcast(&condition); pthread_mutex_unlock(&gate);
    audio_stream_begin();
    pthread_mutex_lock(&gate); restart_done = true; pthread_cond_broadcast(&condition); pthread_mutex_unlock(&gate);
    return NULL;
}
int main(void) {
    s_ring_lock = &chunk; probe_kick = true; audio_stream_begin(); assert(kicks == 1);
    static uint8_t packet[5000]; assert(audio_stream_write_ima(packet, IMA_HEADER_BYTES + FRAME_BYTES) == FRAME_BYTES);
    audio_stream_end(); // A's only frame is also its final frame
    pthread_t a, b; assert(!pthread_create(&a, NULL, read_a, NULL));
    pthread_mutex_lock(&gate);
    while (!decoding) pthread_cond_wait(&condition, &gate);
    pthread_mutex_unlock(&gate);
    assert(s_rd == s_wr && s_output_in_flight && !audio_stream_drained());
    assert(!pthread_create(&b, NULL, begin_b, NULL));
    pthread_mutex_lock(&gate);
    while (!restart_started) pthread_cond_wait(&condition, &gate);
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 1;
    while (!restart_done && !pthread_cond_timedwait(&condition, &gate, &until)) {}
    assert(!restart_done); // without reader's mutex B would reset while A still decodes
    release_decode = true; pthread_cond_broadcast(&condition); pthread_mutex_unlock(&gate);
    pthread_join(a, NULL); pthread_join(b, NULL);
    assert(s_output_in_flight); // restart must not forget A's mixed frame
    output_written();
    assert(restart_done && s_rd == 0 && s_wr == 0 && s_played_bytes == 0 && !s_ended && s_active);
    assert(audio_stream_write_ima(packet, sizeof packet) > 0 && s_rd == 0);
    audio_stream_stop(); ring_release_if_idle(); assert(!s_ring_cap && !s_active);
    allocation_limit = 0; allocations = 0; probe_kick = false; audio_stream_begin(); // buffer refusal is explicitly not playback
    assert(!s_active && audio_stream_write_ima(packet, sizeof packet) == 0);
    audio_stream_end(); assert(s_rd == s_wr && s_played_bytes == 0);
    allocation_limit = 2; allocations = 0; audio_stream_begin();
    assert(!s_active && !s_ring_cap); // Partial allocation must release all blocks and refuse the whole stream.
    for(int i=0;i<RING_BLOCKS;i++)assert(!s_ring[i]);
    allocation_limit = 3; allocations = 0; probe_kick = true; audio_stream_begin();
    assert(allocations == 3); // three medium reservations, fitting beside Live's LP sender stack
    assert(s_active && s_ring_cap == 10800);
    { // Patterned copies crossing the ring end, including a cursor beyond one lap.
        uint8_t input[1301], output[1301];
        for(size_t i=0;i<sizeof input;i++)input[i]=(uint8_t)(i*37+11);
        const size_t positions[]={RING_BLOCK_BYTES-317,2*RING_BLOCK_BYTES-317,RING_BYTES-317,2*RING_BYTES-317};
        for(size_t i=0;i<sizeof positions/sizeof positions[0];i++) {
            ring_copy(positions[i],input,sizeof input,true);
            memset(output,0,sizeof output);
            ring_copy(positions[i],output,sizeof output,false);
            assert(!memcmp(input,output,sizeof input));
        }
        assert(s_rd==0 && s_wr==0); // copying alone must not move published cursors
    }
    static uint8_t wire_packet[1203]; // 100 ms IMA + predictor header
    int16_t samples[480]; int32_t mix[480] = {0};
    for (int i = 0; i < 5; i++) assert(audio_stream_write_ima(wire_packet, sizeof wire_packet) == 1200);
    assert(!read_speech_frame(samples, mix)); // 500 ms cannot start a 600 ms prebuffer.
    assert(audio_stream_write_ima(wire_packet, sizeof wire_packet) == 1200);
    assert(read_speech_frame(samples, mix)); output_written();
    audio_stream_begin();
    assert(audio_stream_write_ima(wire_packet, sizeof wire_packet) == 1200);
    now = s_start_deadline_us - 1; assert(!read_speech_frame(samples, mix));
    now++; // Short answer starts without waiting for a provider end marker.
    for(int i=0;i<5;i++) { assert(read_speech_frame(samples,mix)); output_written(); }
    assert(!read_speech_frame(samples,mix));
    assert(!read_speech_frame(samples,mix)); // quiet terminal tail
    audio_stream_end(); assert(!read_speech_frame(samples,mix) && !s_gaps && !s_gap_frames);
    audio_stream_begin();
    for(int i=0;i<6;i++)assert(audio_stream_write_ima(wire_packet,sizeof wire_packet)==1200);
    for(int i=0;i<30;i++) { assert(read_speech_frame(samples,mix)); output_written(); }
    for(int i=0;i<5;i++)assert(!read_speech_frame(samples,mix));
    for(int i=0;i<3;i++)assert(audio_stream_write_ima(wire_packet,sizeof wire_packet)==1200);
    assert(read_speech_frame(samples,mix) && s_gaps==1 && s_gap_frames==5); // true mid-speech starvation remains visible
    output_written(); audio_stream_begin();
    for(int i=0;i<6;i++)assert(audio_stream_write_ima(wire_packet,sizeof wire_packet)==1200);
    assert(audio_stream_write_ima(wire_packet,IMA_HEADER_BYTES+24)==24);
    for(int i=0;i<30;i++) { assert(read_speech_frame(samples,mix)); output_written(); }
    for(int i=0;i<5;i++)assert(!read_speech_frame(samples,mix));
    audio_stream_end(); assert(read_speech_frame(samples,mix)); output_written();
    assert(!s_gaps && !s_gap_frames); // preexisting partial terminal tail is not new speech
    audio_stream_begin();
    for (int i = 0; i < 9; i++) assert(audio_stream_write_ima(wire_packet, sizeof wire_packet) == 1200);
    assert(audio_stream_write_ima(wire_packet, sizeof wire_packet) == 0); // full packet refused on overflow
    audio_stream_end();
    while (read_speech_frame(samples, mix)) {
        assert(s_output_in_flight && !audio_stream_drained());
        now += 20000; output_written();
    }
    assert(s_played_bytes == 10800);
    assert(s_rd == s_wr && !audio_stream_drained()); // empty ring is not speaker completion
    now = s_output_tail.until - 1; assert(!audio_stream_drained());
    now++; assert(audio_stream_drained());
    audio_stream_stop(); ring_release_if_idle(); assert(!s_ring_cap && !s_active);
    s_doze = true; now = 1000; audio_output_tail_mark(&s_output_tail, now);
    assert(AUDIO_OUTPUT_TAIL_US == 80000);
    assert(!may_doze()); now = s_output_tail.until - 1; assert(!may_doze());
    now++; assert(may_doze()); // ring drained but DMA gets its full configured tail first
    self_audible = true; assert(!may_doze()); self_audible = false;
    effect = true; assert(!may_doze()); effect = false;
    s_active = true; assert(!may_doze()); s_active = false;
    s_gate = true; assert(!may_doze()); s_gate = false;
    s_capture.rx_enabled = true; assert(!may_doze()); s_capture.rx_enabled = false;
    s_capture.adc_enabled = true; assert(!may_doze()); s_capture.adc_enabled = false;
    s_mic_transition = true; assert(!may_doze()); s_mic_transition = false; assert(may_doze());
    puts("audio ring: restart exclusion; fixed capacity; allocation refusal, no partial playback");
}
'''
for name in ("RING_BYTES",):
    mock = mock.replace(f"@{name}@", re.search(rf"#define {name} (\d+)", audio)[1])
for name in ("PREBUF_BYTES", "REBUF_BYTES"):
    mock = mock.replace(f"@{name}@", re.search(rf"#define {name} (\([^\n]+\))", audio)[1])
with tempfile.TemporaryDirectory(prefix="kubik-audio-stream-") as tmp:
    source = Path(tmp) / "audio.c"
    source.write_text(mock + state + reader + power_mock + doze + test)
    exe = Path(tmp) / "audio"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-pthread", "-fsanitize=address,undefined", "-I", str(ROOT / "firmware/main"),
                    str(source), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
