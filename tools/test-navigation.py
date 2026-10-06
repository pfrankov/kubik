#!/usr/bin/env python3
"""Exercise the real BOOT debounce and Back routes with native state mocks."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def section(name, start, end):
    source = (ROOT / 'firmware/main' / name).read_text()
    return source[source.index(start):source.index(end)]


source = section('input.c', 'static void update_boot_key(', 'static void update_power_key(')
source += section('input.c', 'static void refresh_touch_activity(', 'static void update_touch(')
source += section('app_events.c', 'static void handle_boot_short(', 'static void handle_boot_long(')
mocks = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { PIN_BOOT, EV_BOOT_SHORT, EV_BOOT_LONG, EV_TOUCH_DOWN, SFX_MENU_CLOSE };
typedef struct { bool boot, boot_long_sent; int boot_count; int64_t boot_started_ms, touch_activity_ms; } input_state_t;
static bool down, s_menu, s_setup, s_pair_hidden;
static char s_pair_code[17];
static int short_events, long_events, contacts, backs, closes, wakes, dismissals, s_power;
static int g_face;
static int gpio_get_level(int pin) { assert(pin == PIN_BOOT); return !down; }
static void app_post(int event, int a, int b) {
    assert(!a && !b);
    if (event == EV_TOUCH_DOWN) contacts++;
    else if (event == EV_BOOT_SHORT) short_events++;
    else { assert(event == EV_BOOT_LONG); long_events++; }
}
static bool power_is_dark(int state) { return state == 1; }
static void wake(bool sound) { assert(!sound); s_power = 0; wakes++; }
static void setup_leave(void) { closes++; s_setup = false; }
static void audio_sfx(int sound) { assert(sound == SFX_MENU_CLOSE); }
static void menu_back(void) { backs++; }
static void face_lock(void) {}
static void face_unlock(void) {}
static void face_card(int *face, const char *text) { assert(face == &g_face && !text); dismissals++; }
static void disp_wake(void) {}
'''
tests = r'''
static void edge(input_state_t *state, bool pressed, int64_t at) {
    down = pressed;
    for (int i = 0; i < 3; i++) update_boot_key(state, at + i * 10);
}
static void keys(void) {
    input_state_t state = {0};
    down = true; update_boot_key(&state, 0); // bounce cannot become a press
    down = false; update_boot_key(&state, 10);
    assert(!state.boot && !short_events && !long_events);
    edge(&state, true, 100); edge(&state, false, 200);
    assert(short_events == 1 && !long_events);
    edge(&state, true, 300);
    update_boot_key(&state, 1120); assert(!long_events); // threshold is strictly over 800ms
    update_boot_key(&state, 1130); update_boot_key(&state, 5000);
    assert(long_events == 1);
    edge(&state, false, 5100); assert(short_events == 1); // release cannot also go Back
    edge(&state, true, 5200); edge(&state, false, 5300);
    assert(short_events == 2 && long_events == 1);
}
static void routes(void) {
    s_power = 1; s_menu = true; handle_boot_short();
    assert(!wakes && !backs && s_power == 1); // dark BOOT is ignored
    s_power = 0; handle_boot_short(); assert(backs == 1);
    s_setup = true; handle_boot_short(); assert(closes == 1 && backs == 1);
    strcpy(s_pair_code, "PENDING"); handle_boot_short();
    assert(s_pair_hidden && !strcmp(s_pair_code, "PENDING") && backs == 1);
    handle_boot_short(); assert(backs == 2); // dismissed pairing no longer blocks Settings
    s_menu = false; handle_boot_short(); assert(dismissals == 1);
    handle_boot_short(); assert(dismissals == 2); // no volume mutation or network request
}
int main(void) {
    input_state_t touching = {0};
    refresh_touch_activity(&touching, 199); assert(!contacts);
    refresh_touch_activity(&touching, 200); assert(contacts == 1);
    refresh_touch_activity(&touching, 399); assert(contacts == 1);
    refresh_touch_activity(&touching, 400); assert(contacts == 2);
    keys(); routes();
    puts("navigation: BOOT debounce, one long event, no release-short; wake, setup, pairing, menu and card routes passed");
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-navigation-') as tmp:
    src, exe = Path(tmp) / 'navigation.c', Path(tmp) / 'navigation'
    src.write_text(mocks + source + tests)
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
