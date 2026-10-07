#!/usr/bin/env python3
"""Real automatic-turn policy and FIR conversion, with memory/undefined-behavior checks."""
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="c6-wake-word-") as directory:
    binary = pathlib.Path(directory) / "wake-test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "firmware/main"),
                    str(ROOT / "firmware/sim/wake_voice_test.c"),
                    str(ROOT / "firmware/main/auto_voice.c"),
                    str(ROOT / "firmware/main/wake_resample.c"), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Exercise the actual app lifecycle with failed allocation/inference and route replacement.
app = (ROOT / 'firmware/main/app_wake.c').read_text()
power_source = (ROOT / 'firmware/main/app_state.c').read_text()
power_active = power_source[power_source.index('bool power_is_active('):power_source.index('// ------------------------------------------------------------------ talk')]
eligibility = app[app.index('static bool eligible('):app.index('static void suspend_model(')]
lifecycle = app[app.index('static void start_listening('):app.index('void app_wake_recording_stop(')]
upload = app[app.index('static bool start_upload('):app.index('void app_wake_event(')]
wake_callback = app[app.index('static void wake_frame('):app.index('static void start_listening(')]
transport = app[app.index('void app_wake_transport_busy('):app.index('void app_wake_suspend(')]
fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "wake_resample.h"
#include "auto_voice.h"
static bool s_failed, s_listening, s_automatic, s_pending, healthy=true;
static bool model_ok, gate_ok=true;
static unsigned s_attempts, starts, stops, aborts;
static int64_t s_retry_at, clock_ms;
static uint32_t s_epoch, capture_epoch=1, s_auto_session=4, current_session=4;
static int s_lock, s_gen=-1, s_talk=0, s_power=0, s_srv;
static bool s_online=true, mic_allowed=true, s_setup, s_menu, s_act_own;
static atomic_bool s_transport_busy, s_retry_reset;
static char s_pair_code[16];
static int64_t s_power_off_at;
#define TALK_IDLE 0
#define SS_IDLE 0
#define VOICE_LIVE 2
static int voice_mode;
static bool live_active;
static bool app_voice_live_active(void) {return live_active;}
typedef int power_state_t;
#define PWR_AWAKE 0
#define PWR_DIMMED 1
static bool app_agent_mic_allowed(void) {return mic_allowed;}
static wake_resample_t s_resample;
static int16_t s_pcm[640];
static auto_voice_t s_voice;
static int s_auto_turn=42, s_vad, events, vad_calls;
static bool send_ok=true, json_ok=true, json_sent, mic_started;
static int uploaded[4], uploaded_n, capture_started, s_turn=42;
#define MIC_FRAME_SAMPLES 960
#define VAD_SPEECH 1
#define EV_VOICE_END 2
#define EV_VOICE_WAKE 3
static const int16_t *preroll;
static const uint8_t *link_mic_duplex_preroll(const int16_t *pcm) {preroll=pcm;return (const uint8_t*)pcm;}
static bool link_send_duplex_in_session(int tag,const uint8_t *ima,uint32_t session) {
    assert(tag==42 && session==s_auto_session && ima==(const uint8_t*)preroll);
    if(json_sent && send_ok) {assert(uploaded_n<4);uploaded[uploaded_n++]=preroll[0];}
    return send_ok;
}
static bool link_send_mic_in_session(int tag, const int16_t *pcm, int bytes, uint32_t session,const uint8_t *ima) {
    assert(ima==NULL); assert(tag==42 && bytes==1920 && session==s_auto_session);
    if(json_sent && send_ok) {assert(uploaded_n<4);uploaded[uploaded_n++]=pcm[0];}
    return send_ok;
}
static int vad_process(int vad, const int16_t *pcm, int rate, int ms) {
    (void)vad; (void)pcm; assert(rate==16000 && ms==20); vad_calls++; return 0;
}
static void app_post(int event, int turn, int reason) {
    if(event==EV_VOICE_WAKE) {assert(turn==(int)s_epoch && !reason);return;}
    assert(event==2 && turn==42 && reason==2); events++;
}
static unsigned s_postwake_count, s_postwake_next;
static int16_t s_postwake[2][MIC_FRAME_SAMPLES];
static bool detected;
static bool wake_model_feed(const int16_t *pcm, size_t n) {(void)pcm;(void)n;return detected;}
#define portMAX_DELAY 0xffffffffu
static void xSemaphoreTake(int lock, unsigned wait) {(void)lock;(void)wait;}
static void xSemaphoreGive(int lock) {(void)lock;}
static int64_t now_ms(void) {return clock_ms;}
static bool wake_model_start(void) {++starts;return model_ok;}
static bool wake_model_healthy(void) {return healthy;}
static void wake_model_stop(void) {++stops;}
static bool audio_self_audible(void) {return false;}
static bool audio_sfx_playing(void) {return false;}
static uint32_t audio_capture_epoch(void) {return capture_epoch;}
static uint32_t link_session(void) {return current_session;}

static bool audio_wake_gate(bool open, void (*cb)(const int16_t *, int, const uint8_t *)) {
    (void)cb; if(open) { ++capture_epoch; } return gate_ok;
}
static bool link_send_json_in_session(const char *json, uint32_t session) {
    assert(strstr(json,"ptt") && session==s_auto_session); json_sent=json_ok; return json_ok;
}
static void wifi_set_fast(bool fast) {assert(fast);}
static void disp_wake(void) {}
static bool audio_mic_start_epoch(void (*cb)(const int16_t *,int,const uint8_t *), uint32_t epoch) {
    assert(cb && epoch==capture_epoch && json_sent); mic_started=true;return gate_ok;
}
static void app_voice_capture_failed(void) {}
static void app_voice_capture_started(int turn) {assert(turn==42);capture_started++;}
static bool talk_is_listening(int talk) {return talk==1;}
static void suspend_model(void) {if(s_listening){s_listening=false;wake_model_stop();}}
static void app_wake_suspend(void) {suspend_model();}
static void app_wake_abort(bool server) {assert(server);s_automatic=false;++aborts;}
"""
fixture += r"""
#include <stdarg.h>
static int64_t s_last_touch_or_key, s_ptt_t0;
static int g_face, live_stops;
#define TE_PRESS 1
#define TE_RELEASE_SHORT 2
static void wake(bool sound) {assert(!sound);}
static void face_lock(void) {}
static void face_unlock(void) {}
static void face_card(int *face,const char *text) {assert(face==&g_face && !text);}
static void talk_fire(int event) {assert(event==TE_PRESS || event==TE_RELEASE_SHORT);s_talk=1;}
static void app_voice_prepare_capture(int turn) {assert(turn==42);live_active=true;}
static void send_json(const char *format,...) {
    char text[100];va_list args;va_start(args,format);vsnprintf(text,sizeof text,format,args);va_end(args);
    assert(strstr(text,"automatic") && strstr(text,"true"));json_sent=true;
}
static bool app_voice_start_capture(void) {s_pending=false;mic_started=gate_ok;return gate_ok;}
static void app_voice_live_stop(bool server) {assert(server);live_active=false;mic_started=false;live_stops++;}
"""
cases = r"""
int main(void) {
    assert(eligible());
    voice_mode=VOICE_LIVE; assert(eligible());
    live_active=true; assert(!eligible()); live_active=false;
    voice_mode=1; assert(eligible()); voice_mode=0;
    bool *gates[] = {&s_setup, &s_menu, &s_act_own};
    for (unsigned i=0;i<sizeof gates/sizeof gates[0];i++) {
        *gates[i]=true; assert(!eligible()); *gates[i]=false;
    }
    s_pair_code[0]='A'; assert(!eligible()); s_pair_code[0]=0;
    s_power_off_at=1; assert(!eligible()); s_power_off_at=0;
    s_srv=1; assert(!eligible()); s_srv=0;
    s_gen=1; assert(!eligible()); s_gen=-1;
    s_talk=1; assert(!eligible()); s_talk=0;
    s_power=PWR_DIMMED; assert(eligible()); s_power=3; assert(!eligible()); s_power=PWR_AWAKE;
    s_online=false; assert(!eligible()); s_online=true;
    mic_allowed=false; assert(!eligible()); mic_allowed=true;
    s_transport_busy=true; assert(!eligible()); s_transport_busy=false;
    app_wake_tick(); assert(starts==1 && !s_listening);
    clock_ms=4999; app_wake_tick(); assert(starts==1);
    clock_ms=5000; app_wake_tick(); assert(starts==2);
    clock_ms=9999; app_wake_tick(); assert(starts==2);
    clock_ms=10000; app_wake_tick(); assert(starts==3 && s_failed);
    clock_ms=999999; app_wake_tick(); assert(starts==3);
    s_online=false; app_wake_tick(); assert(!s_failed && !s_attempts && !s_retry_at);
    s_online=model_ok=true; app_wake_tick(); assert(starts==4 && s_listening && s_attempts==1);
    healthy=false; app_wake_tick(); assert(!s_listening && stops==1 && starts==4);
    clock_ms+=4999; app_wake_tick(); assert(starts==4);
    healthy=true; ++clock_ms; app_wake_tick(); assert(starts==5 && s_listening);
    app_wake_tick(); assert(starts==5);
    suspend_model(); reset_retries(); // successful recognition is a new capture purpose
    app_wake_tick(); assert(starts==6 && s_listening && s_attempts==1 && !s_failed);
    s_talk=1; s_automatic=true; ++current_session; assert(!recording_tick() && aborts==1);
    s_automatic=true; s_auto_session=current_session; assert(recording_tick());
    s_gen=1; assert(!recording_tick() && aborts==2);
    s_gen=-1; s_talk=0; s_automatic=true; assert(!recording_tick() && aborts==3);
    int16_t frame_pcm[960]={0};
    s_automatic=true; s_pending=false; send_ok=false; automatic_frame(frame_pcm,960,NULL);
    assert(s_pending && events==1 && !vad_calls && !s_voice.elapsed_ms);
    s_pending=false; send_ok=true; automatic_frame(frame_pcm,960,NULL);
    assert(vad_calls==2 && s_voice.elapsed_ms==40 && events==1);
    puts("wake lifecycle: bounded retries, health recovery and old-session capture abort passed");
    s_automatic=false; s_talk=0; suspend_model();
    s_attempts=3; s_failed=true; s_retry_at=clock_ms+5000;
    unsigned before=starts;
    app_wake_transport_busy(true); app_wake_transport_busy(false);
    assert(s_failed && s_attempts==3); // transport task leaves app-owned counters untouched
    app_wake_tick(); assert(starts==before+1 && s_listening && s_attempts==1 && !s_failed);
    app_wake_transport_busy(true); assert(!s_listening);
    before=starts; app_wake_tick(); assert(starts==before && !s_attempts);
    app_wake_transport_busy(false); app_wake_tick(); assert(starts==before+1 && s_listening);
    puts("wake transport: short reconnect pulses reset retry budget in app task");
    s_listening=true; s_pending=false; s_postwake_count=s_postwake_next=0;
    frame_pcm[0]=11; wake_frame(frame_pcm,960,NULL); assert(!s_postwake_count);
    detected=true; frame_pcm[0]=22; wake_frame(frame_pcm,960,NULL);
    assert(s_pending && !s_postwake_count); // Detection frame never enters upload.
    frame_pcm[0]=33; wake_frame(frame_pcm,960,NULL);
    frame_pcm[0]=44; wake_frame(frame_pcm,960,NULL);
    assert(s_postwake_count==2 && s_postwake[0][0]==33 && s_postwake[1][0]==44);
    frame_pcm[0]=55; wake_frame(frame_pcm,960,NULL);
    assert(s_postwake_count==2 && s_postwake[s_postwake_next][0]==44 && s_postwake[(s_postwake_next+1)%2][0]==55);
    s_pending=false; json_sent=false; json_ok=true; uploaded_n=0; capture_started=0; mic_started=false;
    assert(start_upload(capture_epoch));
    assert(uploaded_n==2 && uploaded[0]==44 && uploaded[1]==55 && mic_started && capture_started==1);
    json_ok=false;json_sent=false;uploaded_n=0;mic_started=false;capture_started=0;
    assert(!start_upload(capture_epoch) && !uploaded_n && !mic_started && !capture_started);
    json_ok=true;send_ok=false;
    assert(!start_upload(capture_epoch) && !uploaded_n && !mic_started && !capture_started);
    send_ok=true;gate_ok=false;
    assert(!start_upload(capture_epoch) && mic_started && !capture_started);
    gate_ok=true;start_listening();assert(!s_postwake_count && !s_postwake_next && !s_pending);
    puts("wake privacy: post-detection upload order, bounded ring and startup failure checks passed");
    s_turn=41; s_talk=0; current_session=s_auto_session; send_ok=gate_ok=true;
    s_postwake_count=2;s_postwake_next=0;s_postwake[0][0]=44;s_postwake[1][0]=55;
    uploaded_n=0;mic_started=false;json_sent=false;
    int before_vad=vad_calls;
    start_live_wake();
    assert(live_active && mic_started && uploaded_n==2 && vad_calls==before_vad);
    assert(uploaded[0]==44 && uploaded[1]==55 && s_talk==1);
    s_turn=41;send_ok=false;start_live_wake();
    assert(!live_active && !mic_started && live_stops==1);
    s_turn=41;send_ok=true;gate_ok=false;uploaded_n=0;start_live_wake();
    assert(!live_active && !mic_started && live_stops==2);
    puts("Live wake: actual start branch preserves post-activation frames, bypasses VAD and closes on failure");

}
"""
with tempfile.TemporaryDirectory(prefix='c6-wake-lifecycle-') as directory:
    source = pathlib.Path(directory) / 'lifecycle.c'
    binary = pathlib.Path(directory) / 'lifecycle'
    source.write_text(fixture + power_active + eligibility + transport + wake_callback + lifecycle + upload + cases)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'firmware/main'), str(source), str(ROOT / 'firmware/main/wake_resample.c'), str(ROOT / 'firmware/main/auto_voice.c'), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
