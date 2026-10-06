#!/usr/bin/env python3
"""Actual native input policy: pause, session/capture scoping, mode and allocation failures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = '\n'.join(line for line in (ROOT / 'firmware/main/app_voice.c').read_text().splitlines()
                   if not line.startswith('#include'))
events = (ROOT / 'firmware/main/app_events.c').read_text()
source += events[events.index('static void handle_error_event('):events.index('static void handle_set_event(')]
fixture = r'''
#include <assert.h>
#define app_journal_event(e) ((void)(e))
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "auto_voice.h"
#include "wake_resample.h"
#include "voice_mode.h"
typedef struct {double valuedouble; int valueint; const char *valuestring;} cJSON;
static cJSON field;
static cJSON *cJSON_GetObjectItem(cJSON *j,const char *name) {(void)j;(void)name;return &field;}
static const char *cJSON_GetStringValue(cJSON *j) {return j->valuestring;}
static bool cJSON_IsNumber(cJSON *j) {return j && !j->valuestring;}
static bool cJSON_IsBool(cJSON *j) {(void)j;return false;}
static bool cJSON_IsTrue(cJSON *j) {(void)j;return false;}
static bool agent_protocol_parse_capabilities(cJSON *j,bool *stt,bool *tts) {(void)j;*stt=true;*tts=true;return true;}
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define EV_INPUT_END 1
#define EV_AGENT_CAPS 2
#define EV_SRV_SPEAK_CANCEL 3
#define MIC_FRAME_SAMPLES 960
#define VAD_MODE_3 3
#define VAD_SPEECH 1
#define TALK_HOLD 1
#define TALK_AWAITING 3
#define TE_RELEASE_LONG 3
#define TE_LATCH_TIMEOUT 4
#define TE_ABORT 5
#define BUB_STOP 0
#define STR_NOT_HEARD 0
#define SFX_NOT_HEARD 0
typedef void *vad_handle_t;
static bool alloc_ok=true, voiced;
static unsigned allocations, frees;
static vad_handle_t vad_create_with_param(int mode,int rate,int ms,int a,int b) {
    assert(mode==3&&rate==16000&&ms==20&&a==20&&b==20);allocations++;return alloc_ok?(void*)1:NULL;
}
static int vad_process(vad_handle_t v,const int16_t *pcm,int rate,int ms) {
    (void)pcm;assert(v&&(rate==16000)&&(ms==20));return voiced;
}
static void vad_destroy(vad_handle_t v) {assert(v);frees++;}
static uint32_t session=4, epoch=7;
static unsigned stops, uploaded, ptt_off, posts;
static int event_turn,event_reason,event_type;
static uint32_t event_session;
static int s_turn=9,s_talk=TALK_HOLD,s_think_cues;
static int64_t s_await_t0;
static bool open, muted, gate_ok=true, early_stop;
static bool audio_mic_is_open(void) { return open; }
static void (*callback)(const int16_t*,int,const uint8_t*);
static uint32_t link_session(void) {return session;}
static uint32_t audio_capture_epoch(void) {return epoch;}
static uint32_t audio_mic_dropped_ms(void) {return 0;}
static void request_end(uint8_t turn,uint32_t session);
static bool close_ok=true;
static unsigned input_acks;
static bool audio_mic_gate(bool enabled,void (*cb)(const int16_t*,int,const uint8_t*)) {
    open=enabled&&gate_ok;callback=cb;epoch++;
    if(enabled&&early_stop) {early_stop=false;request_end(s_turn,session);}
    return enabled ? gate_ok : close_ok;
}
static void audio_mic_request_stop(void) {open=false;epoch++;stops++;}
static bool audio_mic_request_stop_epoch(uint32_t expected) {
    if(expected!=epoch)return false;audio_mic_request_stop();return true;
}
static void app_post_in_session(int type,int turn,int reason,uint32_t route) {
    event_type=type;event_turn=turn;event_reason=reason;event_session=route;posts++;
}
static bool link_send_duplex_in_session(uint8_t turn,const uint8_t *ima,uint32_t session) {(void)turn;(void)ima;(void)session;return true;}

static void audio_mic_set_duplex(bool enabled) {(void)enabled;}
static bool link_send_mic_in_session(int turn,const int16_t *pcm,int bytes,uint32_t route,const uint8_t *ima) {
    assert(ima==(const uint8_t *)1); (void)pcm;assert(turn==s_turn&&bytes==1920);if(route!=session)return false;uploaded++;return true;
}
static bool link_send_json_in_session(const char *json,uint32_t route) {if(strstr(json,"live_input_ack"))input_acks++;assert(route==session);return true;}
static bool audio_self_audible(void) {return muted;}
static bool talk_is_listening(int talk) {return talk==1||talk==2;}
static void talk_fire(int why) {s_talk=why==TE_ABORT?0:3;}
static int64_t now_ms(void) {return 1000;}
static void disp_wake(void) {}
static int g_face;
static void face_lock(void) {}
static void face_unlock(void) {}
static void face_card(int *f, const char *s) {(void)f;(void)s;}
static void stop_speech(bool notify) {(void)notify;}
static void app_wake_recording_stop(void);
void app_voice_recording_stop(void);
static void app_wake_recording_stop(void) {app_voice_recording_stop();}
static void app_agent_set_capabilities(bool stt,bool tts) {assert(stt&&tts);}
static void finish_turn(int why) {audio_mic_gate(false,NULL);app_voice_recording_stop();talk_fire(why);ptt_off++;}
static void bubble(int kind,int text,float seconds) {(void)kind;(void)text;(void)seconds;}
static void audio_sfx(int sound) {(void)sound;}
'''
fixture += r'''
#define ESP_LOGW(...) ((void)0)
#define TE_RESOLVE 6
#define ERR_STT_EMPTY 0
#define ERR_VOICE 3
#define ERR_BUSY 4
#define ERR_UNAUTHORIZED 5
#define SFX_THINK 1
#define SFX_ERROR 2
#define BUB_NOT_HEARD 1
#define BUB_BUSY 2
#define BUB_KEY 3
#define BUB_ERROR 4
#define STR_NOT_ALLOWED 1
#define STR_TRY_AGAIN 2
#define EMO_THINKING 1
#define EMO_SAD 2
#define FEV_NOT_HEARD 1
#define FEV_FAIL 2
static int64_t s_last_not_heard;
typedef struct {int a;} app_ev_t;
static bool app_wake_recording(void) {return false;}
static void app_wake_abort(bool server) {(void)server;}
static void face_ev(int kind,int x,int y) {(void)kind;(void)x;(void)y;}
static void face_emo(int kind,float seconds) {(void)kind;(void)seconds;}
'''
cases = r'''
static bool start_capture(void) {app_voice_prepare_capture(s_turn);return app_voice_start_capture();}
int main(void) {
    assert(app_voice_mode()==VOICE_CLASSIC);
    assert(start_capture()&&open&&callback==classic_frame&&!allocations);
    app_voice_capabilities(1,(VOICE_REALTIME<<1)|1);assert(app_voice_mode()==VOICE_REALTIME);
    s_talk=TALK_HOLD;assert(start_capture()&&open&&allocations==1);
    int16_t pcm[960]={0};voiced=true;
    for(int n=0;n<5;n++) callback(pcm,960,(const uint8_t *)1);
    voiced=false;for(int n=0;n<20;n++) callback(pcm,960,(const uint8_t *)1);
    assert(!open&&posts==1&&event_type==EV_INPUT_END&&event_turn==(s_turn|(1<<8))&&event_reason==(int)epoch);
    assert(event_session==session&&uploaded==25);
    app_voice_input_end(event_turn,event_reason,event_session);
    assert(ptt_off==1&&frees==1&&s_talk==3);
    s_talk=TALK_HOLD;assert(start_capture());
    app_voice_input_end(s_turn,epoch,session+1);assert(open);
    app_voice_input_end(s_turn+1,epoch,session);assert(open);
    app_voice_input_end(s_turn,epoch-1,session);assert(open);
    app_voice_input_end(s_turn,epoch,session);assert(!open&&frees==2&&s_talk==3&&!s_think_cues&&s_await_t0==1000);
    s_talk=TALK_HOLD;assert(start_capture());
    unsigned before=stops, previous_posts=posts;uint32_t old_epoch=epoch;epoch++;
    field=(cJSON){.valuedouble=s_turn,.valueint=s_turn};
    assert(app_voice_receive(&field,"input_end"));assert(stops==before&&old_epoch+1==epoch&&posts==previous_posts);
    app_voice_capture_started(s_turn);assert(app_voice_receive(&field,"input_end"));assert(stops==before+1);
    app_voice_input_end(s_turn,epoch,session);
    s_talk=TALK_HOLD;gate_ok=false;assert(!start_capture()&&!open&&frees==4);gate_ok=true;alloc_ok=false;assert(!start_capture()&&!open&&frees==4);
    alloc_ok=true;s_talk=TALK_HOLD;assert(start_capture());
    app_voice_capabilities(1,1);assert(app_voice_mode()==VOICE_CLASSIC&&!open&&frees==5&&s_talk==0);
    unsigned cancel_posts=posts; field=(cJSON){.valuedouble=7,.valueint=7};
    assert(app_voice_receive(&field,"speak_cancel"));
    assert(posts==cancel_posts+1 && event_type==EV_SRV_SPEAK_CANCEL && event_turn==7 && event_session==session);
    field=(cJSON){.valuedouble=0,.valueint=0}; app_voice_receive(&field,"speak_cancel");
    field=(cJSON){.valuedouble=256,.valueint=256}; app_voice_receive(&field,"speak_cancel");
    assert(posts==cancel_posts+1); // RX queues cancellation; it must never send its ACK under the route lock.
    field=(cJSON){.valuestring="realtime"};assert(app_voice_receive(&field,"capabilities"));
    assert(event_type==EV_AGENT_CAPS&&event_reason==3&&event_session==session);
    app_voice_capabilities(1,(VOICE_REALTIME<<1)|1);s_talk=TALK_HOLD;
    previous_posts=posts;early_stop=true;
    assert(start_capture()&&!open&&posts==previous_posts+1&&event_reason==(int)epoch);
    app_voice_input_end(event_turn,event_reason,event_session);assert(frees==6&&s_talk==3);
    s_talk=TALK_HOLD;app_voice_prepare_capture(++s_turn);previous_posts=posts;before=allocations;
    request_end(s_turn,session);assert(posts==previous_posts&&!open);
    assert(app_voice_start_capture()&&!open&&allocations==before&&posts==previous_posts+1);
    app_voice_input_end(event_turn,event_reason,event_session);assert(s_talk==3);
    early_stop=false; s_talk=TALK_HOLD;
    assert(start_capture()); previous_posts=posts;
    native_frame(NULL,0,NULL); assert(posts==previous_posts+1 && !open);
    assert(event_turn==(s_turn|(2<<8)) && event_reason==(int)epoch);
    app_voice_input_end(event_turn,event_reason,event_session); assert(s_talk==0 && !s_vad);
    app_voice_capabilities(1,1); s_talk=TALK_HOLD; assert(start_capture());
    unsigned prior_uploaded=uploaded; uint32_t captured_session=session;
    session++; callback(pcm,960,(const uint8_t *)1);
    assert(uploaded==prior_uploaded && !open && event_session==captured_session);
    assert(start_capture()); captured_session=session; session++;
    callback(NULL,0,NULL); // Overflow belongs to the capture session too.
    assert(!open && event_session==captured_session && uploaded==prior_uploaded);
    app_voice_capabilities(1,(VOICE_REALTIME<<1)|1);s_talk=TALK_HOLD;
    assert(start_capture());unsigned before_acks=input_acks;close_ok=false;
    live_input(s_turn,false,session);assert(input_acks==before_acks && !open && !s_vad && s_talk==0);
    close_ok=true;s_talk=TALK_HOLD;assert(start_capture());
    live_input(s_turn,false,session);assert(input_acks==before_acks+1);
    assert(!open && !app_voice_live_active() && s_talk==TALK_AWAITING && !s_vad);
    live_input(s_turn,true,session);assert(!open); // one-shot mode cannot be resumed by the Live control
    app_voice_capabilities(1,(VOICE_LIVE<<1)|1); s_talk=TALK_HOLD;
    unsigned initial_allocations=allocations;
    assert(start_capture() && app_voice_live_active() && !app_voice_live_ready());
    for(int n=0;n<2500;n++) callback(pcm,960,(const uint8_t*)1);
    assert(open && allocations==initial_allocations); // Neither silence nor the 60s utterance timer closes Live.
    live_input(s_turn,false,session+1); assert(open);
    live_input(s_turn+1,false,session); assert(open);
    before_acks=input_acks;close_ok=false;live_input(s_turn,false,session);
    assert(input_acks==before_acks && app_voice_live_active());
    close_ok=true;live_input(s_turn,false,session); assert(!open && app_voice_live_active() && app_voice_live_ready());
    live_input(s_turn,true,session); assert(open && app_voice_live_active() && app_voice_live_ready() && allocations==initial_allocations);
    live_input(s_turn,false,session); assert(!open);
    request_end(s_turn,session); // Server end must close even while capture is already paused.
    app_voice_input_end(event_turn,event_reason,event_session); assert(!app_voice_live_active() && !open);
    s_talk=TALK_HOLD; assert(start_capture()); app_voice_live_stop(true); assert(!app_voice_live_active() && !open);
    app_voice_capabilities(1,(VOICE_REALTIME<<1)|1);s_talk=TALK_HOLD;assert(start_capture());
    handle_error_event(&(app_ev_t){.a=ERR_VOICE});
    assert(!open && !s_vad && s_talk==0); // A lost off request still closes on the host voice_failed timeout.
    app_voice_capabilities(1,(VOICE_LIVE<<1)|1);s_talk=TALK_HOLD;assert(start_capture());
    handle_error_event(&(app_ev_t){.a=ERR_VOICE});assert(!open && !app_voice_live_active());
    puts("native voice: Realtime VAD/epochs, persistent Live pause/resume/end, classic contrast and allocation failures passed");
}
'''
with tempfile.TemporaryDirectory(prefix='c6-native-input-') as directory:
    path = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    path.write_text(fixture + source + cases)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'firmware/main'), str(path), str(ROOT / 'firmware/main/auto_voice.c'),
                    str(ROOT / 'firmware/main/wake_resample.c'), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
