// Application state machine: owns the conversation state and drives the face.
#pragma once

#include <stdint.h>
#include <stdatomic.h>
#include "face.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef enum {
    EV_PTT_DOWN = 1,
    EV_PTT_UP,
    EV_BOOT_SHORT,
    EV_BOOT_LONG,
    EV_PWR_SHORT,
    EV_PWR_LONG,     // PWR held 2 s: say goodbye and power off
    EV_TAP,
    EV_PET,
    EV_SHAKE,
    EV_PICKUP,
    EV_LINK_UP,
    EV_LINK_DOWN,
    EV_SRV_WELCOME,  // a = volume or -1
    EV_SRV_STATE,    // a = srv_state_t
    EV_SRV_EMOTION,  // a = emotion, b = ms
    EV_SRV_SPEAK,    // a = gen, b = 1 if notify
    EV_SRV_SPEAK_END,  // a = gen
    EV_SRV_SPEAK_CANCEL, // a = gen; flush and ACK outside the WebSocket RX callback
    EV_SRV_ERROR,    // a = error code
    EV_SRV_SET,      // a = volume or -1, b = brightness or -1
    EV_SETUP_START,  // a = setup_reason_t
    EV_PAIR,         // OpenClaw sent a pairing code (app_pair_code())
    EV_SRV_TEXT,     // text card arrived (s_text_rx); b = 1 if notify
    EV_SRV_ACTIVITY, // what OpenClaw is busy with: a = own, b = other (activity_t)
    EV_SRV_CRON,     // cron jobs: a = running, b = seconds to the next one-shot job (-1 = none)
    EV_SWIPE_UP,     // closes it
    EV_DRAG,         // menu open: finger at a = x | first << 16, b = y (panel px)
    EV_MENU_HOLD,    // stationary finger held in Settings: a=x, b=y
    EV_TOUCH_DOWN,   // a finger landed (a dimmed screen brightens at once, before the tap completes)
    EV_VOICE_WAKE,   // a = capture epoch; reserved delivery, Tess only
    EV_VOICE_END,    // a = auto turn, b = empty/timeout; reserved delivery
    EV_INPUT_END,    // a = turn | reason<<8, b = stopped capture epoch; session scoped
    EV_AGENT_CAPS,   // a = STT available, b = TTS available (from the post-welcome capabilities frame)
} app_ev_type_t;

// Agent activity, the categories of OpenClaw's status reactions (ACT_NONE = nothing running).
typedef enum { ACT_NONE = 0, ACT_THINKING, ACT_TOOL, ACT_CODING, ACT_WEB, ACT_DEPLOY, ACT_BUILD, ACT_CONCIERGE,
               ACT_COMPACTING, ACT_STALL, ACT_COUNT } activity_t;
typedef enum { SS_IDLE = 0, SS_LISTENING, SS_TRANSCRIBING, SS_THINKING, SS_SPEAKING } srv_state_t;
typedef enum { ERR_STT_EMPTY = 0, ERR_STT_FAILED, ERR_AGENT, ERR_VOICE, ERR_BUSY, ERR_UNAUTHORIZED, ERR_OTHER } srv_err_t;

extern face_t g_face;
extern SemaphoreHandle_t g_face_mtx;

extern volatile bool g_perf_flush;  // "sim" "stats": the display task logs its frame statistics now
extern atomic_bool g_mode_report;  // "sim" "mode": acknowledge the next presented frame, even if unchanged
void app_start(void);
void app_post(app_ev_type_t type, int a, int b);
// Display requests, applied by the display task (the panel IO is not shared).
void disp_brightness_fade(int level, int ms);  // glides there over ms
void disp_power(bool on);
void disp_wake(void);  // draw the next frame now instead of at the next 30 fps slot (an input changed what the screen shows)
void disp_reinit(void);  // power-cycle and re-initialise the panel
// Light sleep between wakeups: allowed only while dark and idle on battery with radio off; USB keeps the CPU awake.
void power_light_sleep_allow(bool allow);
bool power_light_sleep_enabled(void);
// Called from the display task each frame, under g_face_mtx.
void app_face_inputs(face_t *f);
