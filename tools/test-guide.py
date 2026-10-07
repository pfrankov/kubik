#!/usr/bin/env python3
"""Exercise the real first-use guide policy without Wi-Fi, audio or an agent call."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = '\n'.join(line for line in (ROOT / 'firmware/main/app_guide.c').read_text().splitlines()
                   if not line.startswith('#include'))
menu_source = (ROOT / 'firmware/main/app_menu.c').read_text()
def menu_function(start, end):
    return menu_source[menu_source.index(start):menu_source.index(end)]
source += menu_function('static bool agent_screen_navigation(', 'static bool agent_selection(')
source += menu_function('void menu_open(void)', '// `sound`:')
mocks = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "face.h"
#include "face_agent.h"
#include "app_state.h"
#include "settings.h"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
enum { SFX_PAGE, SFX_MENU_OPEN };
enum { SS_IDLE, SS_THINKING };
static face_t g_face;
settings_t g_settings;
static bool s_menu, s_setup, s_online, s_pair_hidden;
static int s_menu_drag_row;
typedef struct { bool unlocked; } menu_service_t;
static menu_service_t s_service;
static char s_pair_code[17];
static talk_state_t s_talk;
static int s_srv, s_gen, s_act_own;
static int64_t s_power_off_at, s_menu_touch_ms;
static struct { int icon; } s_next_bubble;
static int opened, writes, wakes, wake_aborts, wake_suspends;
static void app_wake_abort(bool tell_server) { assert(tell_server); wake_aborts++; }
static void app_wake_suspend(void) { wake_suspends++; }
static void app_voice_live_stop(bool tell_server) { assert(tell_server); }
static bool write_fails;
static void face_lock(void) {}
static void face_unlock(void) {}
static void disp_wake(void) { wakes++; }
static int64_t now_ms(void) { return 1000; }
static void menu_sync(void) {}
static void request_agent_options(uint8_t cursor) { agent_menu_request_options(&g_face.agent, cursor, 1000); }
void menu_open(void);
static void input_set_menu(bool on) { if (on) opened++; }
static void menu_press(int row, int part) { (void)row; (void)part; }
static void wake(bool sound) { (void)sound; }
static void audio_sfx(int sound) { (void)sound; }
static void menu_close(bool sound) { (void)sound; s_menu = false; agent_menu_hide(&g_face.agent); }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "test failure"; }
esp_err_t settings_complete_guide(void) {
    if (g_settings.guide_done) return ESP_OK;
    writes++;
    if (write_fails) return ESP_FAIL;
    g_settings.guide_done = true; return ESP_OK;
}
'''
tests = r'''
static void reset(void) {
    memset(&g_face, 0, sizeof g_face); memset(&g_settings, 0, sizeof g_settings);
    strcpy(g_settings.wifi_ssid, "existing-network"); strcpy(g_settings.server_url, "kubik://existing.local");
    g_settings.volume = 42;
    s_menu = s_setup = s_online = s_offered = write_fails = s_pair_hidden = false;
    s_pair_code[0] = 0; s_talk = TALK_IDLE; s_srv = SS_IDLE; s_gen = -1;
    s_act_own = 0; s_power_off_at = 0; opened = writes = wakes = wake_aborts = wake_suspends = 0;
}
static void test_first_start(void) {
    reset(); app_guide_tick(); assert(!opened);
    s_online = true; app_guide_tick(); assert(!opened); // welcome is insufficient without caps
    agent_menu_set_capabilities(&g_face.agent, false, false);
    app_guide_tick(); assert(opened == 1 && s_menu && wakes && g_face.agent.view == AGENT_VIEW_GUIDE);
    assert(g_face.agent.volume == 42 && !g_settings.guide_done && !writes);
    menu_close(false); app_guide_tick(); assert(opened == 1); // no repeated interruptions in this boot
    s_offered = false; app_guide_tick(); assert(opened == 2); // interrupted tour returns on a new boot
}
static void test_back_does_not_complete(void) {
    reset(); app_guide_start(); app_guide_tap(AGENT_HIT_BACK);
    assert(s_menu && !g_face.agent.open && !g_settings.guide_done && !writes);
    app_guide_start(); app_guide_tap(AGENT_HIT_GUIDE_NEXT); app_guide_tap(AGENT_HIT_BACK);
    assert(g_face.agent.open && !g_face.agent.guide_step && !writes);
}
static void test_menu_return_and_pairing(void) {
    reset(); strcpy(s_pair_code, "PENDING"); menu_open(); assert(!s_menu && wake_aborts == 1 && wake_suspends == 1);
    s_pair_hidden = true; g_face.agent.online = true; menu_open(); assert(wake_aborts == 2 && wake_suspends == 2); assert(s_menu && !strcmp(s_pair_code, "PENDING"));
    for (int view = AGENT_VIEW_MODELS; view <= AGENT_VIEW_MODELS; view++) {
        g_face.agent.open = true; g_face.agent.view = view;
        g_face.agent.request_pending = true; // Back does not cancel a server save.
        menu_back(); assert(s_menu && g_face.agent.open && g_face.agent.view == AGENT_VIEW_OVERVIEW);
        assert(g_face.agent.request_pending);
        menu_back(); assert(s_menu && !g_face.agent.open);
    }
    menu_back(); assert(!s_menu && !writes);
    s_pair_code[0] = 0;
    app_guide_start(); menu_back(); assert(s_menu && !g_face.agent.open && !writes);
    app_guide_start(); app_guide_tap(AGENT_HIT_GUIDE_NEXT); menu_back();
    assert(g_face.agent.open && !g_face.agent.guide_step && !writes);
}
static void test_busy(void) {
    for (int blocked = 0; blocked < 9; blocked++) {
        reset(); s_online = true; agent_menu_set_capabilities(&g_face.agent, true, true);
        switch (blocked) {
        case 0: s_setup = true; break;
        case 1: s_menu = true; break;
        case 2: s_pair_code[0] = 'A'; break;
        case 3: s_talk = TALK_HOLD; break;
        case 4: s_srv = SS_THINKING; break;
        case 5: s_gen = 1; break;
        case 6: s_act_own = 1; break;
        case 7: s_power_off_at = 1; break;
        case 8: g_face.card_n = 1; break;
        }
        app_guide_tick(); assert(!opened && !writes);
    }
}
static void test_completion_and_replay(void) {
    reset(); s_online = true; agent_menu_set_capabilities(&g_face.agent, true, false);
    settings_t before = g_settings; app_guide_start();
    for (int step = 1; step <= 3; step++) { app_guide_tap(AGENT_HIT_GUIDE_NEXT); assert(g_face.agent.guide_step == step); }
    app_guide_tap(AGENT_HIT_BACK); assert(g_face.agent.guide_step == 2);
    app_guide_tap(AGENT_HIT_GUIDE_NEXT);
    write_fails = true; app_guide_tap(AGENT_HIT_GUIDE_DONE);
    assert(!g_settings.guide_done && g_face.agent.open && g_face.agent.guide_save_failed);
    write_fails = false; app_guide_tap(AGENT_HIT_GUIDE_DONE);
    assert(g_settings.guide_done && s_menu && !g_face.agent.open);
    before.guide_done = true; assert(!memcmp(&before, &g_settings, sizeof before));
    int count = writes; app_guide_start(); assert(g_face.agent.guide_step == 0);
    app_guide_tap(AGENT_HIT_GUIDE_SKIP); assert(!s_menu && writes == count);
    s_offered = false; app_guide_tick(); assert(!s_menu); // completed guide stays dismissed after reboot
}
int main(void) {
    test_first_start(); test_back_does_not_complete(); test_menu_return_and_pairing(); test_busy(); test_completion_and_replay();
    puts("guide: authenticated capabilities, busy gates, no repeat, completion failure, replay and settings preservation passed");
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-guide-') as tmp:
    src, exe = Path(tmp) / 'guide.c', Path(tmp) / 'guide'
    src.write_text(mocks + source + tests)
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g', '-fsanitize=address,undefined',
                    '-Ifirmware/main', '-Ifirmware/sim/settings_stubs', str(src),
                    'firmware/main/event_journal.c', 'firmware/main/agent_menu.c', 'firmware/main/app_state.c', '-o', str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)], check=True)
