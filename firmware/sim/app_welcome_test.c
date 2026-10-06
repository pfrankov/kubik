// Execute the production welcome transition with observable UI side effects.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "face.h"
#include "ui_text.h"
#define OUTAGE_MS 30000
#define ESP_LOGI(...) ((void)0)
enum { SFX_SETUP_OK, SFX_HELLO, SFX_CONNECT };
typedef struct { int a, b; } app_ev_t;
static bool s_online, s_pair_hidden, s_outage, s_ever_online;
static int s_srv_progress, s_offline_icon, displayed;
static char s_pair_code[17];
static int64_t s_boot_ms, clock_ms;
static struct { int volume; bool greeted; } g_settings;
#define link_via() "wifi"
static int64_t now_ms(void) { return clock_ms; }
static void audio_set_volume(int v) { assert(v >= 0 && v <= 100); }
static void audio_sfx(int v) { (void)v; }
static void face_emo(emotion_t e, float t) { (void)e; (void)t; }
static void bubble(bubble_icon_t icon, int text, float t) { (void)text; (void)t; displayed = icon; }
static void bubble_after(bubble_icon_t icon, int text, float t) { (void)icon; (void)text; (void)t; }
static void menu_sync(void) {}
static void app_agent_refresh_if_open(void) {}
static void device_state_volume_report(bool force) { assert(force); }
static void settings_save(void) {}
static void app_journal_event(const app_ev_t *e) { (void)e; }
/* PRODUCTION_WELCOME */
int main(void) {
    app_ev_t e = {.a=70};
    // Previously offline is indefinite, but a first early welcome has no new bubble.
    clock_ms = 25000; g_settings.greeted = true;
    s_offline_icon = displayed = BUB_NO_SERVER;
    handle_welcome_event(&e);
    assert(s_online && s_ever_online && !s_offline_icon && displayed == BUB_NONE);
    // An ordinary welcome must preserve unrelated transient feedback.
    displayed = BUB_TALK; handle_welcome_event(&e); assert(displayed == BUB_TALK);
    // A long outage still gets the existing Connected acknowledgement.
    s_outage = true; s_offline_icon = displayed = BUB_NO_SERVER;
    handle_welcome_event(&e); assert(displayed == BUB_OK && !s_outage);
    // Pairing still takes precedence over reconnection feedback.
    s_pair_code[0] = 'X'; s_offline_icon = displayed = BUB_NO_SERVER;
    handle_welcome_event(&e); assert(displayed == BUB_OK && !s_pair_code[0]);
    puts("welcome: stale offline cleared; ordinary, long-outage and pairing feedback preserved");
}
