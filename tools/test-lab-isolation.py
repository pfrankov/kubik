#!/usr/bin/env python3
"""Compile the real firmware lab wrapper with deterministic app state and native UI."""
from pathlib import Path
import runpy
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "firmware/main/app_lab.c").read_text()
source = source.replace('#include "app_lab.h"', '').replace('#include "app_internal.h"', '').replace('#include "audio.h"', '').replace('#include "audio_sfx.h"', '')
app = (ROOT / "firmware/main/app.h").read_text()
events = app[app.index('typedef enum {'):app.index('} app_ev_type_t;') + len('} app_ev_type_t;')]
mock = r'''
#include "screen_lab.h"
#include "app_mailbox.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static face_t g_face;
static bool s_menu, live, listening, fail_alloc;
static int s_srv, locks, wakes, s_act_own;
static int s_gen=-1;
static bool playing;
static bool audio_stream_playing(void) { return playing; }
static int quiet_calls, sound_calls;
static void audio_sfx_quiet(void) { assert(locks == 1); quiet_calls++; }
static void audio_tess_cue(tess_cue_t cue, float strength, float position) { assert(locks == 1 && (cue == TC_IMPACT || cue == TC_SWING)); sound_calls++; }
static int64_t s_menu_touch_ms;
#define SS_IDLE 0
static int s_talk;
static void face_lock(void) { assert(!locks); locks++; }
static void face_unlock(void) { assert(locks==1); locks--; }
static void disp_wake(void) { wakes++; }
static int64_t now_ms(void) { return 1000; }
static bool app_voice_live_active(void) { return live; }
static bool talk_is_listening(int state) { (void)state; return listening; }
static void *test_calloc(size_t n, size_t size) { return fail_alloc ? NULL : calloc(n,size); }
#define calloc test_calloc
'''
test = r'''
int main(void) {
 face_init(&g_face);
 assert(!app_lab_open());
 s_menu=true; live=true; assert(!app_lab_open()); live=false;
 listening=true; assert(!app_lab_open()); listening=false;
 s_srv=1; assert(!app_lab_open()); s_srv=0;
 s_gen=0; assert(!app_lab_open()); s_gen=-1;
 playing=true; assert(!app_lab_open()); playing=false;
 s_act_own=1; assert(!app_lab_open()); s_act_own=0;
 fail_alloc=true; assert(!app_lab_open() && !app_lab_active()); fail_alloc=false;
 assert(app_lab_open() && app_lab_active());
 assert(!app_lab_open()); // no second allocation survives
 screen_lab_select(s_lab, LAB_SETTINGS);
 int initial=g_face.menu.volume;
 assert(app_lab_event(&(app_ev_t){.type=EV_TAP,.a=72,.b=100}));
 assert(s_lab->volume>80 && g_face.menu.volume==initial);
 assert(app_lab_event(&(app_ev_t){.type=EV_PTT_DOWN})); // never real recording
 assert(app_lab_event(&(app_ev_t){.type=EV_PTT_UP}));
 assert(app_lab_event(&(app_ev_t){.type=EV_SWIPE_UP}));
 screen_lab_select(s_lab,LAB_EVENT_LOG);
 assert(app_lab_event(&(app_ev_t){.type=EV_DRAG,.a=240|(1<<16),.b=370}));
 assert(app_lab_event(&(app_ev_t){.type=EV_DRAG,.a=240,.b=140}));
 assert(app_lab_event(&(app_ev_t){.type=EV_SWIPE_UP}));
 assert(s_lab->face.journal.open && s_lab->face.journal.offset==3);
 assert(app_lab_event(&(app_ev_t){.type=EV_TAP,.a=240,.b=140}));
 assert(!s_lab->face.journal.detail);
 assert(app_lab_event(&(app_ev_t){.type=EV_TAP,.a=240,.b=173}));
 assert(s_lab->face.journal.detail);
 assert(app_lab_event(&(app_ev_t){.type=EV_BOOT_SHORT}));
 assert(!s_lab->face.journal.detail && s_lab->face.journal.open);
 screen_lab_select(s_lab,LAB_VOICE);
 assert(app_lab_event(&(app_ev_t){.type=EV_DRAG,.a=240|(1<<16),.b=300}));
 assert(app_lab_event(&(app_ev_t){.type=EV_DRAG,.a=240,.b=330}));
 assert(app_lab_event(&(app_ev_t){.type=EV_TAP,.a=240,.b=300}));
 assert(s_lab->face.agent.view==AGENT_VIEW_VOICE_MODES);
 assert(!app_lab_event(&(app_ev_t){.type=EV_LINK_DOWN})); // real network events still pass
 g_face.grav_x=.4f; g_face.grav_y=.8f; g_face.jolt_dvx=.1f;
 g_face.rub_in=(rub_input_t){.down=true,.x=240,.y=240,.path=20};
 scene_t scene; face_lock(); assert(app_lab_frame(&scene,1.f/30,true)); face_unlock();
 assert(s_lab->face.grav_x==.4f && s_lab->face.grav_y==.8f && !g_face.rub_in.path && !g_face.jolt_dvx);
 screen_lab_select(s_lab,LAB_SETTINGS);
 g_face.rub_in=(rub_input_t){.down=true,.x=72,.y=200};
 face_lock(); app_lab_frame(&scene,1.f/30,true); face_unlock(); assert(s_lab->finger.active);
 g_face.rub_in.down=false;
 face_lock(); app_lab_frame(&scene,1.f/30,true); face_unlock(); assert(!s_lab->finger.active);
 screen_lab_select(s_lab,LAB_OFFLINE);
 app_lab_play_cue(TC_IMPACT,.5f,0); assert(sound_calls == 1);
 app_lab_play_cue(TC_TOUCH,.5f,0); assert(sound_calls == 1);
 screen_lab_select(s_lab,LAB_ECHO_0);
 app_lab_play_cue(TC_SWING,.5f,0); assert(sound_calls == 2);
 app_lab_play_cue(TC_SAD,.5f,0); assert(sound_calls == 2);
 s_lab->face.dark=true; app_lab_play_cue(TC_SWING,.5f,0); assert(sound_calls == 2);
 s_lab->face.dark=false;
 screen_lab_select(s_lab,LAB_SPEAK);
 app_lab_play_cue(TC_IMPACT,.5f,0); assert(sound_calls == 2);
 app_lab_play_cue(TC_SWING,.5f,0); assert(sound_calls == 2);
 screen_lab_select(s_lab,-1);
 assert(app_lab_event(&(app_ev_t){.type=EV_BOOT_SHORT}));
 assert(!app_lab_active() && s_menu && !s_lab && !g_face.rub_in.down);
 assert(app_lab_open()); assert(!app_lab_event(&(app_ev_t){.type=EV_PWR_SHORT})); assert(!app_lab_active());
 assert(app_lab_open()); assert(!app_lab_event(&(app_ev_t){.type=EV_BOOT_LONG})); assert(!app_lab_active());
 face_lock(); assert(!app_lab_frame(&scene,1.f/30,true)); face_unlock();
 int stopped = quiet_calls; assert(stopped > 0);
 int sounds = sound_calls; app_lab_play_cue(TC_IMPACT,.5f,0); assert(sound_calls == sounds);
 g_face.rub_in.down=true; app_lab_close(); app_lab_close(); assert(!locks && g_face.rub_in.down && quiet_calls == stopped);
 puts("lab isolation: busy/allocation gates, ephemeral sliders, KEY interception, network pass-through, motion and exits passed");
}
'''
sources = runpy.run_path(str(ROOT / "tools/test-frames.py"))["SOURCES"]
sources = [item for item in sources if not item.startswith("sim/")]
sources += ["main/screen_lab", "main/screen_lab_events", "main/screen_lab_fixture", "main/screen_lab_draw"]
with tempfile.TemporaryDirectory(prefix="kubik-lab-isolation-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(mock + events + source + test)
    subprocess.run(["cc", "-O1", "-g", "-fsanitize=address,undefined", "-Ifirmware/main",
                    "-Ifirmware/managed_components/espressif__qrcode", str(path / "test.c"),
                    *[f"firmware/{item}.c" for item in sources], "-lm", "-o", str(path / "test")], cwd=ROOT, check=True)
    subprocess.run([str(path / "test")], check=True)
