#!/usr/bin/env python3
"""Actual menu hold gesture: one event, no activation on a drag/home, no release tap."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / 'firmware/main/input.c').read_text()
source = source[source.index('static void update_touch_gesture('):source.index('static void refresh_touch_activity(')]
menu = (ROOT / 'firmware/main/app_menu.c').read_text()
source += menu[menu.index('void menu_hold('):]
mock = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "input_hold.h"
enum {EV_MENU_HOLD, EV_SWIPE_UP, EV_PET, EV_TAP};
typedef struct { bool touching, gesture_done; int touch_x0, touch_y0; float touch_path; int64_t touch_started_ms; } input_state_t;
enum { MENU_VOLUME, SFX_PAGE };
static struct { struct { bool open; } status; struct { bool open; } journal; struct { bool open; } agent; struct { bool sound; } menu; } g_face;
static bool s_menu = true;
static struct { bool ready; } s_lab_unlock;
static bool screen_lab_entry(bool ready, int x, int y) { return ready && x>=20 && x<=260 && y>=12 && y<=56; }
static bool app_lab_open(void) { return true; }
static int s_menu_drag_row = -1, opens;
static int64_t s_menu_touch_ms;
static void face_lock(void) {} static void face_unlock(void) {}
static int face_menu_hit(void *face, int x, int y, int *part) { (void)face;(void)y;*part=0;return x==72?MENU_VOLUME:-1; }
static int64_t now_ms(void) { return 1000; }
static void audio_sfx(int sound) { assert(sound==SFX_PAGE); opens++; }
static void menu_sync(void) {} static void disp_wake(void) {}
static int events[4];
static void app_post(int type, int x, int y) { (void)x; (void)y; events[type]++; }
static void update_menu_drag(input_state_t *state, int x, int y, bool menu) {(void)state;(void)x;(void)y;(void)menu;}
'''
test = r'''
int main(void) {
 g_face.status.open=true; s_menu_drag_row=MENU_VOLUME; menu_hold(72,200); assert(!g_face.menu.sound);
 g_face.status.open=false; s_menu_drag_row=-1;
 menu_hold(72,200); assert(!g_face.menu.sound); // touch began before menu was ready
 s_menu_drag_row=MENU_VOLUME; menu_hold(188,200); assert(!g_face.menu.sound);
 menu_hold(72,200); assert(g_face.menu.sound && opens==1 && s_menu_drag_row==-1);
 menu_hold(72,200); assert(opens==1);
 input_state_t s = {.touching=true,.touch_x0=72,.touch_y0=200};
 update_touch_gesture(&s,72,200,799,true); assert(!events[EV_MENU_HOLD]);
 update_touch_gesture(&s,72,200,800,true); update_touch_gesture(&s,72,200,2000,true);
 assert(events[EV_MENU_HOLD]==1); release_touch(&s,2100); assert(!events[EV_TAP]);
 s=(input_state_t){.touching=true,.touch_x0=72,.touch_y0=200,.touch_path=40};
 update_touch_gesture(&s,72,220,900,true); assert(events[EV_MENU_HOLD]==1);
 s=(input_state_t){.touching=true,.touch_x0=72,.touch_y0=200};
 release_touch(&s,200); assert(events[EV_TAP]==1);
 s=(input_state_t){.touching=true,.touch_x0=72,.touch_y0=200};
 update_touch_gesture(&s,72,200,900,false); assert(events[EV_MENU_HOLD]==1 && events[EV_PET]==1);
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-menu-hold-') as directory:
    p=Path(directory); (p/'test.c').write_text(mock+source+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Ifirmware/main','-fsanitize=address,undefined',str(p/'test.c'),'-lm','-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('menu hold: threshold, single event, drag/home rejection and tap contrast passed')
