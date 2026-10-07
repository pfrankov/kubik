// Run production posting and event dispatch with a bounded FreeRTOS queue stub.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_mailbox.h"

/* PRODUCTION_EVENTS */

#define TAG "app-post-test"
#define OUTAGE_MS 30000
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define pdPASS 1

#define BUB_NONE 0
#define BUB_OK 1
#define BUB_TALK 2
#define STR_PAIRED 3
#define STR_HOLD_TO_TALK 4
#define STR_CONNECTED 5
#define SFX_SETUP_OK 1
#define SFX_HELLO 2
#define SFX_CONNECT 3
#define EMO_JOY 1
#define EMO_HAPPY 2

typedef struct {
    unsigned count;
    app_ev_t events[32];
} fake_queue_t;

typedef fake_queue_t *QueueHandle_t;
static QueueHandle_t s_q;
static void *s_app_task;
static portMUX_TYPE s_mailbox_mux = 0;
static app_mailbox_t s_mailbox;
static fake_queue_t s_queue;
static bool s_online, s_pair_hidden, s_outage, s_ever_online;
static int s_srv_progress, s_offline_icon, displayed;
static char s_pair_code[17];
static int64_t s_boot_ms;
static struct { int volume; bool greeted; } g_settings;
static bool s_power_off_failed;
static int64_t s_power_off_at;
static const char *s_route = "none";
static uint32_t s_session = 42;
static unsigned s_down_calls;

static int64_t now_ms(void) { return 60000; }
static const char *link_via(void) { return s_route; }
static uint32_t link_session(void) { return s_session; }

static int xQueueSend(QueueHandle_t queue, const app_ev_t *event, int wait) {
    (void)wait;
    if (!queue || queue->count == 32) return 0;
    queue->events[queue->count++] = *event;
    return pdPASS;
}

static int xQueueReceive(QueueHandle_t queue, app_ev_t *event, int wait) {
    (void)wait;
    if (!queue || !queue->count) return 0;
    *event = queue->events[0];
    memmove(queue->events, queue->events + 1, (--queue->count) * sizeof(*event));
    return 1;
}

static void xTaskNotifyGive(void *task) { (void)task; }
static void handle_link_down(void) { s_down_calls++; s_online = false; }
static void handle_pair_event(void) {}
static void app_journal_event(const app_ev_t *event) { (void)event; }
static void audio_set_volume(int volume) { (void)volume; }
static void audio_sfx(int sound) { (void)sound; }
static void face_emo(int emotion, float duration) { (void)emotion; (void)duration; }
static void bubble(int icon, int text, float duration) {
    (void)text;
    (void)duration;
    displayed = icon;
}
static void bubble_after(int icon, int text, float duration) {
    (void)icon;
    (void)text;
    (void)duration;
}
static void menu_sync(void) {}
static void app_agent_refresh_if_open(void) {}
static void device_state_volume_report(bool force) { (void)force; }
static void settings_save(void) {}
static void handle_cron_event(const app_ev_t *event) { (void)event; }
static void handle_state_event(const app_ev_t *event) { (void)event; }
static void handle_activity_event(const app_ev_t *event) { (void)event; }
static void handle_emotion_event(const app_ev_t *event) { (void)event; }
static void handle_speak_event(const app_ev_t *event) { (void)event; }
static void handle_text_event(const app_ev_t *event) { (void)event; }
static void handle_speak_end_event(const app_ev_t *event) { (void)event; }
static void handle_speak_cancel_event(const app_ev_t *event) { (void)event; }
static void handle_error_event(const app_ev_t *event) { (void)event; }
static void handle_set_event(const app_ev_t *event) { (void)event; }
static void handle_agent_capabilities(const app_ev_t *event) { (void)event; }
static bool handle_control_event(const app_ev_t *event) { (void)event; return false; }
static bool is_touch_event(uint8_t type) { return type == EV_TAP; }
static void handle_touch_event(const app_ev_t *event) { (void)event; }
static bool intercept_input(const app_ev_t *event) { (void)event; return false; }
static void app_wake_event(uint32_t value) { (void)value; }
static void app_wake_end_event(int a, int b) { (void)a; (void)b; }
static void app_voice_input_end(int a, int b, uint32_t session) {
    (void)a;
    (void)b;
    (void)session;
}
static void esp_restart(void) {}

/* PRODUCTION_POST */
/* PRODUCTION_TAKE */
/* PRODUCTION_LINK_HANDLER */
/* PRODUCTION_WELCOME_HANDLER */
/* PRODUCTION_REMOTE_CHECK */
/* PRODUCTION_SERVER_HANDLER */
/* PRODUCTION_DISPATCH */

static void app_task_pass(void) {
    app_ev_t e;
    /* PRODUCTION_TASK_DRAIN */
}

static void reset_harness(void) {
    memset(&s_queue, 0, sizeof s_queue);
    memset(&s_mailbox, 0, sizeof s_mailbox);
    s_q = &s_queue;
    s_app_task = NULL;
    s_online = false;
    s_pair_hidden = s_outage = s_ever_online = false;
    s_srv_progress = s_offline_icon = displayed = 0;
    s_pair_code[0] = 0;
    s_boot_ms = 0;
    g_settings.volume = 0;
    g_settings.greeted = true;
    s_power_off_failed = false;
    s_power_off_at = 0;
    s_route = "none";
    s_session = 42;
    s_down_calls = 0;
}

static void saturate_queue(void) {
    app_ev_t noise = {.type = EV_TAP};
    for (int i = 0; i < 32; i++) assert(xQueueSend(s_q, &noise, 0) == pdPASS);
}

static void test_disconnect_survives_full_queue(void) {
    reset_harness();
    s_online = true;
    saturate_queue();
    app_post(EV_LINK_DOWN, 0, 0);
    assert(s_queue.count == 32);
    app_task_pass();
    assert(s_queue.count == 24 && s_down_calls == 1 && !s_online);
}

static void test_replacement_route_ignores_queued_disconnect(void) {
    reset_harness();
    s_online = true;
    saturate_queue();
    app_post(EV_LINK_DOWN, 0, 0);
    s_route = "wifi";
    app_task_pass();
    assert(s_down_calls == 0 && s_online);
}

static void test_welcome_survives_full_queue_and_keeps_session_fence(void) {
    reset_harness();
    saturate_queue();
    app_post_in_session(EV_SRV_WELCOME, 70, 1, s_session);
    assert(s_queue.count == 32);
    app_task_pass();
    assert(s_queue.count == 24 && s_online);

    reset_harness();
    saturate_queue();
    app_post_in_session(EV_SRV_WELCOME, 70, 1, s_session - 1);
    app_task_pass();
    assert(!s_online);
}

static void test_reserved_slots_keep_key_speech_order_and_coalesce(void) {
    reset_harness();
    app_post_in_session(EV_SRV_SPEAK, 7, 0, s_session);
    app_post(EV_PTT_UP, 0, 0);
    app_post(EV_LINK_DOWN, 0, 0);
    app_post_in_session(EV_SRV_WELCOME, 70, 0, s_session - 1);
    app_post_in_session(EV_SRV_WELCOME, 80, 1, s_session);

    app_ev_t event;
    assert(take_critical_event(&event) && event.type == EV_SRV_SPEAK);
    assert(take_critical_event(&event) && event.type == EV_PTT_UP);
    assert(take_critical_event(&event) && event.type == EV_LINK_DOWN);
    assert(take_critical_event(&event) && event.type == EV_SRV_WELCOME);
    assert(event.a == 80 && event.session == s_session);
    assert(!take_critical_event(&event));
}

int main(void) {
    test_disconnect_survives_full_queue();
    test_replacement_route_ignores_queued_disconnect();
    test_welcome_survives_full_queue_and_keeps_session_fence();
    test_reserved_slots_keep_key_speech_order_and_coalesce();
    puts("app-post: reserved disconnect/welcome delivery preserves route, session, and key/speech order");
}
