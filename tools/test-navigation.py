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
source += section('input_touch.c', 'static void refresh_touch_activity(', 'static bool read_sample(')
source += section('app_control.c', 'bool app_offline_play_button(', 'static bool fallen_character(')
source += section('app_events.c', 'static bool boot_home_voice(', 'static void handle_boot_long(')
mocks = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "device_controls.h"
enum { PIN_BOOT, EV_BOOT_SHORT, EV_BOOT_LONG, EV_TOUCH_DOWN, SFX_MENU_CLOSE, SFX_DISMISS, BUB_OK };
typedef struct { bool boot, boot_long_sent; int boot_count; int64_t boot_started_ms, touch_activity_ms; } input_state_t;
typedef input_state_t input_touch_t;
static bool down, s_menu, s_setup, s_pair_hidden, s_offline_play;
static bool s_online = true;
static char s_pair_code[17];
static int short_events, long_events, contacts, backs, closes, wakes, dismissals, s_power;
static struct { unsigned card_n; bool card_hold; } g_face;
static unsigned voice_down, voice_up;
#if KUBIK_VOICE_BUTTON_IS_BOOT
static unsigned confirmed;
static bool live, muse_confirm;
static void ptt_down(void) { voice_down++; live = false; }
static void ptt_up(void) { voice_up++; }
static void ptt_tap(void) { ptt_down(); ptt_up(); }
static bool app_voice_live_active(void) { return live; }
static bool muse_pair_confirm_button(void) { if (!muse_confirm) return false; confirmed++; muse_confirm = false; return true; }
#endif
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
static void audio_sfx(int sound) { assert(sound == SFX_MENU_CLOSE || sound == SFX_DISMISS); }
static void menu_back(void) { backs++; }
static void face_lock(void) {}
static void face_unlock(void) {}
static void face_card(void *face, const char *text) { assert(face == &g_face && !text); dismissals++; g_face.card_n = 0; g_face.card_hold = false; }
#define KUBIK_CHARACTER 1
#define CHARACTER_TESS 1
static bool sky_playing;
static bool tess_games_end_sky(void *face) { assert(face == &g_face); bool was = sky_playing; sky_playing = false; return was; }
static void disp_wake(void) {}
static void bubble_raw(int kind,const char *text,float seconds) { assert(kind==BUB_OK && strstr(text,"Offline play") && seconds>0); }
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
    sky_playing = true; handle_boot_short(); assert(dismissals == 2 && !sky_playing); // a short BOOT ends a sky instead of opening a card
    s_pair_code[0]=0; s_online=false;
    handle_boot_short(); assert(s_offline_play);
    sky_playing=true; handle_boot_short(); assert(!sky_playing && s_offline_play);
    handle_boot_short(); assert(!s_offline_play);
    strcpy(s_pair_code,"PENDING"); assert(!app_offline_play_button());
    s_pair_code[0]=0; s_online=true; assert(!app_offline_play_button());
}
static void voice_home(void) {
    unsigned before = voice_down, ups = voice_up;
    handle_boot_short();
#if KUBIK_VOICE_BUTTON_IS_BOOT
    assert(voice_down == before + 1 && voice_up == ups + 1);
    handle_boot_short(); assert(voice_down == before + 2 && voice_up == ups + 2);
    live = true; handle_boot_short(); assert(!live && voice_down == before + 3 && voice_up == ups + 3);
#else
    assert(voice_down == before && voice_up == ups);
#endif
}
static void back_is_not_voice(void) {
#if KUBIK_VOICE_BUTTON_IS_BOOT
    unsigned before = voice_down;
    g_face.card_n = 1; handle_boot_short(); assert(!g_face.card_n && voice_down == before);
    g_face.card_hold = true; handle_boot_short(); assert(!g_face.card_hold && voice_down == before);
    sky_playing = true; handle_boot_short(); assert(!sky_playing && voice_down == before);
    s_menu = true; handle_boot_short(); assert(voice_down == before); s_menu = false;
    s_setup = true; handle_boot_short(); assert(voice_down == before); s_setup = false;
    strcpy(s_pair_code,"PENDING"); s_pair_hidden = false; handle_boot_short(); assert(voice_down == before);
    handle_boot_short(); assert(voice_down == before); s_pair_code[0] = 0;
    s_online = false; handle_boot_short(); assert(voice_down == before); s_online = true;
#endif
}
static void confirmation_and_dark_gate(void) {
#if KUBIK_VOICE_BUTTON_IS_BOOT
    unsigned before = voice_down;
    muse_confirm = true; handle_boot_short(); assert(confirmed == 1 && voice_down == before);
    s_power = 1; live = true; handle_boot_short(); assert(live && voice_down == before);
    s_power = 0; live = false;
#endif
}
static void shared_voice_button(void) {
    s_pair_code[0] = 0; s_setup = s_menu = s_offline_play = false; s_online = true;
    voice_home(); back_is_not_voice(); confirmation_and_dark_gate();
}
int main(void) {
    input_state_t touching = {0};
    refresh_touch_activity(&touching, 199); assert(!contacts);
    refresh_touch_activity(&touching, 200); assert(contacts == 1);
    refresh_touch_activity(&touching, 399); assert(contacts == 1);
    refresh_touch_activity(&touching, 400); assert(contacts == 2);
    keys(); routes(); shared_voice_button();
    puts("navigation: BOOT debounce, one long event, no release-short; wake, setup, pairing, menu and card routes passed");
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-navigation-') as tmp:
    src, exe = Path(tmp) / 'navigation.c', Path(tmp) / 'navigation'
    src.write_text(mocks + source + tests)
    for s3 in (0, 1):
        subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
                        f'-DCONFIG_IDF_TARGET_ESP32S3={s3}', '-I' + str(ROOT / 'firmware/main'),
                        '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

# A released BOOT is a tap even if codec startup took longer than the physical hold threshold.
source = section('app_control.c', 'void ptt_up(void)', 'void volume_bubble(')
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define ESP_LOGI(...) ((void)0)
enum { TALK_IDLE, TALK_HOLD, TALK_LATCHED, TALK_AWAITING };
enum { TE_RELEASE_SHORT, TE_RELEASE_LONG, SFX_LATCH_START };
#define LATCH_MS 400
static int s_talk;
static int64_t clock_ms, startup_ms, s_ptt_t0, s_last_touch_or_key;
static bool s_ignore_up, live, capture_ok = true;
static unsigned long_releases, latch_sounds;
static int64_t now_ms(void) { return clock_ms; }
static bool app_voice_live_active(void) { return live; }
static void finish_turn(int why) { assert(why == TE_RELEASE_LONG); long_releases++; s_talk = TALK_AWAITING; }
static void talk_fire(int why) { assert(why == TE_RELEASE_SHORT); s_talk = TALK_LATCHED; }
static void audio_sfx(int sound) { assert(sound == SFX_LATCH_START); latch_sounds++; }
static void ptt_down(void) {
    if (live) { live = false; s_ignore_up = true; return; }
    if (s_talk == TALK_LATCHED) { s_talk = TALK_AWAITING; s_ignore_up = true; return; }
    if (!capture_ok) { s_talk = TALK_IDLE; return; }
    s_talk = TALK_HOLD; s_ptt_t0 = now_ms(); clock_ms += startup_ms;
}
'''
cases = r'''
int main(void) {
    startup_ms = 10; ptt_tap(); assert(s_talk == TALK_LATCHED && latch_sounds == 1 && !long_releases);
    ptt_tap(); assert(s_talk == TALK_AWAITING && !s_ignore_up);
    s_talk = TALK_IDLE; startup_ms = 700; ptt_tap();
    assert(s_talk == TALK_LATCHED && latch_sounds == 2 && !long_releases && s_ptt_t0 == clock_ms);
    ptt_tap(); assert(s_talk == TALK_AWAITING && !s_ignore_up);
    s_talk = TALK_IDLE; ptt_down(); clock_ms += 600; ptt_up();
    assert(s_talk == TALK_AWAITING && long_releases == 1); // C6 real holds retain their old release path.
    s_talk = TALK_IDLE; live = true; ptt_tap(); assert(!live && !s_ignore_up && s_talk == TALK_IDLE);
    capture_ok = false; ptt_tap(); assert(s_talk == TALK_IDLE && latch_sounds == 2);
    puts("released BOOT tap: fast/slow startup latches; second tap, Live end, failure and held-key contrast passed");
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-ptt-tap-') as tmp:
    src, exe = Path(tmp) / 'tap.c', Path(tmp) / 'tap'
    src.write_text(fixture + source + cases)
    subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
