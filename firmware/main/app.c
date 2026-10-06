#include "app_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "audio_sfx.h"
#include "board.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "input.h"
#include "ui_text.h"
#include "wifi.h"

static const char *TAG = "app";
const char *const app_activity_names[ACT_COUNT] = {"", "thinking", "tool", "coding", "web", "deploy",
    "build", "concierge", "compacting", "stall"};
bool s_srv_progress;  // the server paces speech by "progress" reports

face_t g_face;
SemaphoreHandle_t g_face_mtx;

QueueHandle_t s_q;

static portMUX_TYPE s_mailbox_mux = portMUX_INITIALIZER_UNLOCKED;
static app_mailbox_t s_mailbox;
static TaskHandle_t s_app_task;

void app_post_in_session(app_ev_type_t type, int a, int b, uint32_t session) {
    app_ev_t event = {.type = (uint8_t)type, .a = a, .b = b, .session = session};
    int slot = type == EV_SRV_SPEAK ? 0 : type == EV_SRV_SPEAK_END ? 1 :
               type == EV_SRV_TEXT ? 2 : type == EV_PTT_UP ? 3 : type == EV_VOICE_WAKE ? 4 : type == EV_VOICE_END ? 5 :
               type == EV_INPUT_END ? 6 : type == EV_AGENT_CAPS ? 7 : -1;
    if (slot >= 0) {
        portENTER_CRITICAL(&s_mailbox_mux);
        app_mailbox_put(&s_mailbox, (unsigned)slot, event);
        portEXIT_CRITICAL(&s_mailbox_mux);
    } else if (s_q && xQueueSend(s_q, &event, 0) != pdPASS) {
        ESP_LOGW(TAG, "event queue full: dropped %u", (unsigned)type);
    }
    if (s_app_task) xTaskNotifyGive(s_app_task);
}

void app_post(app_ev_type_t type, int a, int b) { app_post_in_session(type, a, b, 0); }

static bool take_critical_event(app_ev_t *event) {
    portENTER_CRITICAL(&s_mailbox_mux);
    bool available = app_mailbox_take(&s_mailbox, event);
    portEXIT_CRITICAL(&s_mailbox_mux);
    return available;
}

// ------------------------------------------------------------------ state
bool s_online;           // welcome received on the current link
bool s_ever_online;
uint8_t s_turn;
talk_state_t s_talk;     // the turn: idle, key held, latched, waiting for the reply
bool s_ignore_up;        // the release of the press that stopped a latched turn
int64_t s_ptt_t0;
int64_t s_last_not_heard;
int s_think_cues;
int64_t s_await_t0;
srv_state_t s_srv = SS_IDLE;
int64_t s_last_activity;
int64_t s_last_touch_or_key;
volatile power_state_t s_power = PWR_AWAKE;  // read by the link task too
bool s_setup;
volatile int s_quiet_reads;  // PMIC polls in a row without VBUS (usb_gate_read); 0 until read: no light sleep
volatile bool s_pmic_vbus_known_false;  // pmic_read fails safe to usb_power=true; false therefore proves a good VBUS read
volatile bool s_pmic_charging;
int64_t s_boot_ms;
setup_phase_t s_setup_phase;
int64_t s_setup_done_ms;
bool s_setup_greeted_phone;
setup_reason_t s_setup_reason;
int64_t s_setup_wait_ms;  // last connecting ping
// Pairing: OpenClaw does not know this Kubik yet and showed a code to approve.
portMUX_TYPE s_pair_mux = portMUX_INITIALIZER_UNLOCKED;
char s_pair_rx[17];       // written by the link task
char s_pair_code[17];     // pending pairing (app task)
bool s_pair_hidden;
int64_t s_pair_seen_ms;
int s_offline_icon;       // persistent "why offline" bubble shown
// Reconnects are silent: the link often drops for a moment (server restart, Wi-Fi roam) and
// comes back by itself. Only an outage longer than OUTAGE_MS is told, and then its end too.
int64_t s_down_ms;        // when the session was lost
bool s_outage;            // lost for longer than OUTAGE_MS: the user was told
// Text card from the server (link task -> app task).
portMUX_TYPE s_text_mux = portMUX_INITIALIZER_UNLOCKED;
char s_text_rx[1024];
uint32_t s_text_receipt, s_text_session, s_text_revision;
bool s_text_notify;
int s_status_key;         // ongoing-state bubble shown (recording, thinking)
// What OpenClaw is busy with (activity_t): a run for this Kubik (its question, a heartbeat or cron
// result on its way here) and any other run in the Gateway.
int s_act_own, s_act_other;
int64_t s_power_off_at;   // PWR held: the goodbye is showing, power goes at this time
bool s_power_off_failed;  // the PMIC kept the power on (USB): dark until the next PWR press
int s_status_icon;        // the bubble status_tick put up (see status_tick)
// Settings menu (BOOT held 0.8 s).
bool s_menu, s_menu_dirty;
int64_t s_menu_touch_ms;
int s_menu_drag_row = -1;
int s_menu_tick_level = -1;  // last 10 % step that clicked while sliding
int64_t s_armed_ms;          // reset or power off tapped once; a second tap on it within 3 s confirms
int s_armed_part;            // which one (menu action part)

int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
volatile int s_debug_mode;
volatile int64_t s_debug_network_until;
volatile int64_t s_debug_until;  // "sim" "mode" test hook
volatile int64_t s_debug_rub_until;   // "sim" "rub" test hook: a scripted vigorous rub of the finger
volatile int64_t s_debug_spin_until;  // "sim" "spin" test hook: a scripted turn of the view
volatile bool g_perf_flush;


void face_lock(void) { xSemaphoreTake(g_face_mtx, portMAX_DELAY); }
void face_unlock(void) { xSemaphoreGive(g_face_mtx); }

void face_ev(face_event_t ev, float x, float y) {
    face_lock();
    face_event(&g_face, ev, x, y);
    face_unlock();
}

void bubble(bubble_icon_t icon, int text, float seconds) {
    face_lock();
    face_bubble(&g_face, icon, text >= 0 ? str((str_id_t)text) : NULL, seconds);
    face_unlock();
}

// A bubble with its own words (e.g. "Volume 60%").
void bubble_raw(bubble_icon_t icon, const char *text, float seconds) {
    face_lock();
    face_bubble(&g_face, icon, text, seconds);
    face_unlock();
}

// One bubble waiting for the current timed one to end (e.g. "Paired", then how to talk).
app_bubble_t s_next_bubble;
void bubble_after(bubble_icon_t icon, int text, float seconds) {
    s_next_bubble.icon = icon;
    s_next_bubble.text = text;
    s_next_bubble.seconds = seconds;
}

void face_emo(emotion_t e, float s) {
    face_lock();
    face_set_emotion(&g_face, e, s);
    face_unlock();
}

void send_json(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void send_json(const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    link_send_json(buf);
}

int64_t s_pickup_sfx_at;  // the delighted giggle after the start (0 = none)

// The one place the screen changes power state; the table says what each transition does.
static void prepare_power_transition(power_event_t event, power_state_t next) {
    if (event == PE_IDLE_SLEEP) audio_sfx_quiet();
    if (!power_is_active(next)) {
        app_voice_live_stop(true); app_wake_abort(true); app_wake_suspend();
        if (talk_is_listening(s_talk)) {
            audio_mic_gate(false, NULL);
            app_wake_recording_stop();
            talk_fire(TE_ABORT);
        }
    }
}

void power_fire(power_event_t event, bool sound) {
    power_step_t step = power_step(s_power, event);
    if (!step.legal) return;
    prepare_power_transition(event, step.next);
    int user = g_settings.brightness;
    s_power = step.next;
    input_set_dark(power_is_dark(s_power));
    if (step.next == PWR_AWAKE) s_last_activity = now_ms();
    if (step.actions & PA_SCREEN_ON) disp_power(true);
    if (step.actions & PA_BRIGHT_NOW) disp_brightness_fade(user, 0);
    if (step.actions & PA_FADE_UP) disp_brightness_fade(user, 350);
    if (step.actions & PA_FADE_DIM) disp_brightness_fade(power_brightness(PWR_DIMMED, user), 1500);
    if (step.actions & PA_FADE_SLEEP) disp_brightness_fade(0, DARKEN_MS);
    if (step.actions & PA_FADE_GOODBYE) disp_brightness_fade(0, 1400);
    if (step.actions & PA_SCREEN_OFF) disp_power(false);
    if (step.actions & PA_WAKE_FACE) {
        face_ev(FEV_WAKE, 0, 0);
        if (sound) audio_sfx(SFX_WAKE);
    }
}

void wake_for_reply(void) {
    power_fire(PE_REPLY, false);
}

void wake(bool sound) {
    if (power_is_dark(s_power)) return;
    s_last_activity = now_ms();
    power_fire(PE_WAKE, sound);
}

// Moves the turn along; false when the event means nothing in the current state.
bool talk_fire(talk_event_t event) {
    talk_state_t next = talk_next(s_talk, event);
    bool changed = next != s_talk;
    s_talk = next;
    return changed;
}

// ------------------------------------------------------------------ speech
void stop_speech(bool tell_server) {
    app_speech_stop(tell_server);
}

void finish_turn(talk_event_t why) {
    if (!talk_fire(why)) return;
    audio_mic_gate(false, NULL);
    uint32_t session = app_wake_session();
    app_wake_recording_stop();
    int ms = (int)(now_ms() - s_ptt_t0);
    if (session) {
        char message[80];
        snprintf(message, sizeof message, "{\"t\":\"ptt\",\"on\":false,\"turn\":%d,\"ms\":%d}", s_turn, ms);
        if (!link_send_json_in_session(message, session)) { talk_fire(TE_ABORT); return; }
    } else send_json("{\"t\":\"ptt\",\"on\":false,\"turn\":%d,\"ms\":%d}", s_turn, ms);
    audio_sfx(SFX_LISTEN_STOP);
    s_await_t0 = now_ms();
    s_think_cues = 0;
    ESP_LOGI(TAG, "sent turn %d (%d ms)", s_turn, ms);
}

// Why Kubik cannot talk right now: no Wi-Fi, no server, or a server that
// does not know Kubik (link.c tells which).
int offline_icon(void) {
    if (!wifi_sta_connected()) return BUB_NO_WIFI;
    switch (link_last_problem()) {
    case LINK_NO_PLUGIN: return BUB_PLUGIN;
    case LINK_REFUSED: case LINK_KEY_CHANGED: return BUB_KEY;
    default: return BUB_NO_SERVER;
    }
}
void offline_bubble(float seconds) {
    int icon = offline_icon();
    int key_text = link_last_problem() == LINK_KEY_CHANGED ? STR_SERVER_KEY_CHANGED : STR_NOT_ALLOWED;
    int text = icon == BUB_PLUGIN ? STR_ADD_TO_OPENCLAW : icon == BUB_KEY ? key_text
             : icon == BUB_NO_SERVER ? STR_OPENCLAW_OFFLINE : -1;
    if (icon == BUB_NO_SERVER && !wifi_time_ready() && !strncmp(g_settings.server_url, "wss://", 6))
        text = STR_WAITING_FOR_TIME;
    bubble((bubble_icon_t)icon, text, seconds);
}

// KEY: hold to talk, or press once to start and once more to send.
void setup_enter(setup_reason_t why) {
    app_voice_live_stop(true);
    app_wake_abort(true);
    app_wake_suspend();
    if (s_setup) return;
    wake(false);
    s_setup = true;
    s_setup_phase = SETUP_JOIN;
    s_setup_greeted_phone = false;
    s_setup_reason = why;
    stop_speech(true);
    link_set_wifi_allowed(false);  // Wi-Fi sessions pause; USB keeps working
    if (link_wifi_stopped()) setup_start(why);
    s_setup_wait_ms = 0;
    audio_sfx(SFX_SETUP);
}

void setup_leave(void) {
    face_lock();  // the face points at the setup's QR codes: let go of them before setup_stop frees them
    face_set_setup(&g_face, NULL, 0, 0, NULL, NULL);
    face_unlock();
    setup_stop();
    s_setup = false;
    s_setup_phase = SETUP_OFF;
    link_set_wifi_allowed(true);
}

static bool setup_ready(void) {
    if (setup_active()) return true;
    if (!link_wifi_stopped()) return false;
    setup_start(s_setup_reason); return setup_active();
}
void setup_step(void) {
    if (!setup_ready()) return;
    setup_phase_t p = setup_poll(), old = s_setup_phase;
    if (p == old) {
        if (p == SETUP_TRYING && now_ms() - s_setup_wait_ms > 1800) {
            s_setup_wait_ms = now_ms();
            audio_sfx(SFX_SETUP_WAIT);
        }
        if (p == SETUP_DONE && now_ms() - s_setup_done_ms > 4000) {
            ESP_LOGI(TAG, "setup complete, restarting");
            esp_restart();
        }
        return;
    }
    s_setup_phase = p;
    wake(false);
    switch (p) {
    case SETUP_OFF:  // the lost network came back by itself
        setup_leave();
        audio_sfx(SFX_CONNECT);
        bubble(BUB_OK, -1, 2.5f);
        face_emo(EMO_HAPPY, 2.f);
        break;
    case SETUP_OPEN:
        if (!s_setup_greeted_phone) {
            s_setup_greeted_phone = true;
            audio_sfx(SFX_SETUP_PHONE);
        }
        break;
    case SETUP_TRYING:
        s_setup_wait_ms = now_ms();
        audio_sfx(SFX_SETUP_WAIT);
        break;
    case SETUP_DONE:
        s_setup_done_ms = now_ms();
        audio_sfx(SFX_SETUP_OK);
        face_emo(EMO_JOY, 8.f);
        break;
    case SETUP_FAILED:
        audio_sfx(SFX_SETUP_FAIL);
        face_emo(EMO_SAD, 5.f);
        break;
    default:
        break;
    }
}

// ------------------------------------------------------------------ events
static void power_task(void *arg) {
    while (1) {
        power_status_t p;
        pmic_read(&p);
        s_quiet_reads = usb_gate_read(s_quiet_reads, p.usb_power);
        s_pmic_vbus_known_false = !p.usb_power;
        s_pmic_charging = p.charging;
        face_lock();
        face_set_power(&g_face, p.battery_present, p.battery_pct, p.charging, p.power_known && p.usb_power);
        face_unlock();
        vTaskDelay(pdMS_TO_TICKS(2000));  // also the delay before a plugged-in cable ends light sleep
    }
}

static void app_task(void *arg) {
    app_ev_t e;
    while (1) {
        // Bounded ordinary batch, then the reserved inbox: touches cannot starve delivery or KEY release.
        for (int n = 0; n < 8 && xQueueReceive(s_q, &e, 0); n++) app_handle(&e);
        for (int n = 0; n < APP_MAILBOX_SLOTS && take_critical_event(&e); n++) app_handle(&e);
        app_tick();
        app_wake_tick();
        if (!uxQueueMessagesWaiting(s_q)) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(app_tick_ms()));
    }
}

void app_start(void) {
    s_boot_ms = now_ms();
    s_last_activity = s_boot_ms;
    s_q = xQueueCreate(32, sizeof(app_ev_t));
    ESP_ERROR_CHECK(s_q ? ESP_OK : ESP_ERR_NO_MEM);
    app_speech_init();
    app_wake_init();
    link_handlers_t h;
    app_protocol_handlers(&h);
    link_init(&h);
    input_init();
    ESP_ERROR_CHECK(xTaskCreate(app_task, "app", 4096, NULL, 6, &s_app_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(power_task, "power", 3072, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
