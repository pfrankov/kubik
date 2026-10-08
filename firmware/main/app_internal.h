#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "app_wake.h"
#include "app_voice.h"
#include "app_mailbox.h"
#include "app_state.h"
#include "app_speech.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "link.h"
#include "settings.h"
#include "setup.h"

#define OUTAGE_MS 30000
#define PICKUP_LONG_S 45
#define LATCH_MS 400
#define LATCH_MAX_MS 60000



typedef struct {
    int icon, text;
    float seconds;
} app_bubble_t;

extern bool s_srv_progress;
extern QueueHandle_t s_q;

extern bool s_online, s_ever_online;
extern uint8_t s_turn;
extern talk_state_t s_talk;
extern bool s_ignore_up;
extern int64_t s_ptt_t0, s_last_not_heard;
extern int s_think_cues;
extern int64_t s_await_t0;
extern srv_state_t s_srv;
extern int64_t s_last_activity, s_last_touch_or_key;
extern volatile power_state_t s_power;
extern bool s_setup;
extern volatile int s_quiet_reads;
extern volatile bool s_pmic_vbus_known_false, s_pmic_charging;
extern int64_t s_boot_ms;
extern setup_phase_t s_setup_phase;
extern int64_t s_setup_done_ms;
extern bool s_setup_greeted_phone;
extern setup_reason_t s_setup_reason;
extern int64_t s_setup_wait_ms;
extern portMUX_TYPE s_pair_mux;
extern char s_pair_rx[17], s_pair_code[17];
extern bool s_pair_hidden;
extern int64_t s_pair_seen_ms;
extern int s_offline_icon;
extern int64_t s_down_ms;
extern bool s_outage;
extern portMUX_TYPE s_text_mux;
extern char s_text_rx[1024];
extern uint32_t s_text_receipt, s_text_session, s_text_revision;
extern bool s_text_notify;
extern int s_status_key;
extern int s_act_own, s_act_other;
extern int64_t s_power_off_at;
extern bool s_power_off_failed;
extern int s_status_icon;
extern bool s_menu, s_menu_dirty;
extern int64_t s_menu_touch_ms;
extern int s_menu_drag_row;
extern int s_menu_tick_level;
extern int64_t s_armed_ms;
extern int s_armed_part;
extern app_bubble_t s_next_bubble;
extern int64_t s_pickup_sfx_at;
extern volatile int s_debug_mode;
extern volatile int64_t s_debug_network_until;
int app_network_phase(void);
uint32_t app_network_transitions(void);
extern volatile int64_t s_debug_until;
extern volatile int64_t s_debug_rub_until;
extern volatile int64_t s_debug_spin_until;

int64_t now_ms(void);
void face_lock(void);
void face_unlock(void);
void face_ev(face_event_t ev, float x, float y);
void bubble(bubble_icon_t icon, int text, float seconds);
void bubble_raw(bubble_icon_t icon, const char *text, float seconds);
void bubble_after(bubble_icon_t icon, int text, float seconds);
void face_emo(emotion_t e, float seconds);
void send_json(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void wake(bool sound);
void wake_for_reply(void);
void power_fire(power_event_t event, bool sound);
bool talk_fire(talk_event_t event);
void stop_speech(bool tell_server);
void finish_turn(talk_event_t why);
int offline_icon(void);
void offline_bubble(float seconds);
void ptt_down(void);
void ptt_up(void);
void volume_bubble(int level);
void app_touch_event(const app_ev_t *e);
void setup_enter(setup_reason_t why);
void setup_leave(void);
void setup_step(void);
void power_off_begin(void);
esp_err_t factory_reset(void);
void menu_sync(void);
void menu_open(void);
void menu_back(void);
void menu_close(bool sound);
void menu_tap(int x, int y);
void menu_drag(int x, int y, bool first);
void menu_hold(int x, int y);
void device_state_volume_report(bool force);
bool app_agent_receive_reply(const agent_menu_reply_t *reply);
void app_guide_start(void);
void app_guide_tick(void);
void app_guide_tap(int hit);
void app_agent_set_capabilities(bool stt_available, bool tts_available);
bool app_agent_mic_allowed(void);
void app_agent_refresh_if_open(void);
void app_agent_tick(int64_t now);
void app_handle(const app_ev_t *e);
void app_post_in_session(app_ev_type_t type, int a, int b, uint32_t session);
void app_tick(void);
int app_tick_ms(void);
bool app_games_available(void);
bool app_games_flush_safe(void);
void app_protocol_handlers(link_handlers_t *handlers);

extern const char *const app_activity_names[ACT_COUNT];
