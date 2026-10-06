#!/usr/bin/env python3
"""Production wire/session and text-mailbox boundaries, without hardware or providers."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / 'firmware/main'

def section(file, start, end):
    source = (MAIN / file).read_text()
    return source[source.index(start):source.index(end)]

app = (MAIN / 'app.h').read_text()
enums = app[app.index('typedef enum {'):app.index('extern face_t g_face;')]
wire = section('app_protocol.c', 'static void post_remote(', 'static void handle_activity_json(')
wire += section('app_protocol.c', 'static void handle_text_json(', 'static void handle_cron_json(')
text = section('app_events.c', 'static void handle_text_event(', 'static void handle_speak_end_event(')
text += section('app_journal.c', 'void app_journal_text(', 'static void state(')
route = section('app_events.c', 'static bool remote_current(', 'static void handle_speak_cancel_event(')
route += section('app_events.c', 'static bool handle_server_event(', 'static void handle_agent_capabilities(')
fixture = r'''
#include "face.h"
#include "app_mailbox.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static int s_text_mux, applied, cards, wakes, receipts, locks;
static char s_text_rx[1024];
static uint32_t s_text_receipt, s_text_session, s_text_revision, session=1;
static bool s_text_notify;
static face_t g_face;
static int s_gen=-1;
static int64_t s_last_activity;
static app_ev_t queued;
static const char *const k_states[] = {"idle","listening","transcribing","thinking","speaking"};
static uint32_t link_session(void) { return session; }
static int protocol_volume(cJSON *j,const char *key) {(void)j;(void)key;return -1;}
static void app_post_in_session(int type,int a,int b,uint32_t epoch) {queued=(app_ev_t){.type=type,.a=a,.b=b,.session=epoch};}
static int64_t now_ms(void) {return 1000;}
static void face_lock(void) {assert(!locks); locks++;}
static void face_unlock(void) {assert(locks==1);locks--;}
void face_card(face_t *f,const char *str) {assert(f==&g_face && str && locks);cards++;}
static void wake_for_reply(void) {wakes++;}
static void talk_fire(int event) {(void)event;}
static void audio_sfx(int sound) {(void)sound;}
static bool audio_stream_playing(void) {return false;}
static void face_ev(int event,int a,int b) {(void)event;(void)a;(void)b;}
static bool link_send_json_in_session(const char *str,uint32_t epoch) {assert(str && epoch==session);receipts++;return true;}
void app_journal_text(const char *text,bool notification);
static void handle_cron_event(const app_ev_t *e) {(void)e;applied++;}
static void handle_welcome_event(const app_ev_t *e) {(void)e;applied++;}
static void handle_state_event(const app_ev_t *e) {(void)e;applied++;}
static void handle_activity_event(const app_ev_t *e) {(void)e;applied++;}
static void handle_emotion_event(const app_ev_t *e) {(void)e;applied++;}
static void handle_speak_event(const app_ev_t *e) {(void)e;applied++;}
static void handle_speak_end_event(const app_ev_t *e) {(void)e;applied++;}
static void handle_speak_cancel_event(const app_ev_t *e) {(void)e;applied++;}
static void handle_error_event(const app_ev_t *e) {(void)e;applied++;}
static void handle_set_event(const app_ev_t *e) {(void)e;applied++;}
enum {TE_RESOLVE,SFX_NOTIFY};
'''
tests = r'''
static void receive(const char *str) {cJSON *j=cJSON_Parse(str);assert(j);handle_text_json(j);cJSON_Delete(j);}
int main(void) {
 cJSON *j=cJSON_Parse("{}");handle_welcome_json(j);cJSON_Delete(j);assert(queued.session==1);
 j=cJSON_Parse("{\"s\":\"thinking\"}");handle_state_json(j);cJSON_Delete(j);
 assert(queued.type==EV_SRV_STATE && queued.session==1);
 session=2;assert(handle_server_event(&queued) && !applied && !g_face.journal.count);
 queued.session=2;assert(handle_server_event(&queued) && applied==1);
 receive("{\"text\":\"First\",\"receipt\":1}");app_ev_t old=queued;
 receive("{\"text\":\"Second\",\"receipt\":2}");
 handle_text_event(&old);assert(!cards && !wakes && !receipts && !g_face.journal.count);
 handle_text_event(&queued);assert(cards==1 && wakes==1 && receipts==1 && g_face.journal.count==1);
 assert(!strcmp(event_journal_at(&g_face.journal,0)->text,"Second"));
 receive("{\"text\":\"Obsolete\",\"receipt\":3}");session=3;
 assert(handle_server_event(&queued));assert(cards==1 && receipts==1);
 handle_text_event(&queued);assert(cards==1 && receipts==1);
 receive("{\"text\":\"Reminder\",\"kind\":\"notify\"}");handle_server_event(&queued);
 assert(cards==2 && event_journal_at(&g_face.journal,0)->kind==JOURNAL_NOTICE && !locks);
 puts("journal ingress: source session, obsolete route, replaced text revision, receipt and notification passed");
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-journal-ingress-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(enums + fixture + wire + text + route + tests)
    # Enum definitions need stdint first (face.h supplies portable UI types).
    source = (path / 'test.c').read_text()
    (path / 'test.c').write_text('#include "face.h"\n' + source)
    idf = Path(os.environ.get('IDF_PATH', str(Path.home() / 'esp/esp-idf-v5.5.1')))
    json_dir = idf / 'components/json/cJSON'
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-Wno-deprecated-declarations','-fsanitize=address,undefined','-Ifirmware/main',f'-I{json_dir}',
                    str(path / 'test.c'),'firmware/main/event_journal.c',str(json_dir / 'cJSON.c'),
                    '-lm','-o',str(path / 'test')],cwd=ROOT,check=True)
    subprocess.run([str(path / 'test')],check=True)
