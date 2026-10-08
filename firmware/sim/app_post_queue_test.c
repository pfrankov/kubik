// Run production posting and event dispatch with a bounded FreeRTOS queue stub.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "character.h"
#include "app_mailbox.h"
#include "esp_err.h"

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
#define FEV_TAP 0
#define TESS_GAME_NONE 0
#define TESS_GAME_ECHO 1
#define TESS_GAME_CATCH 2

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
static struct { int volume; bool greeted; uint8_t tess_progress; } g_settings;
static bool s_power_off_failed;
static int64_t s_power_off_at;
typedef struct { uint8_t progress, game; bool available; uint32_t round; } fake_games_t;
typedef struct { fake_games_t tess_games; float idle_t; } fake_face_t;
static fake_face_t g_face;
static int64_t fake_now = 60000, s_tess_flush_retry_at;
static unsigned progress_save_calls;
static unsigned touch_events, queue_count_at_save, link_downs_at_save;
static uint8_t progress_save_mask;
static bool progress_save_fail, progress_flush_safe = true, face_locked, save_while_face_locked;
static esp_err_t reset_result;
static const char *s_route = "none";
static uint32_t s_session = 42;
static unsigned s_down_calls;
static bool game_won, game_card_visible;
static int game_target_x, game_target_y;
static unsigned game_taps, accepted_texts, ordinary_tap_calls, face_tap_calls;
static unsigned gesture_calls, wake_calls;
static uint32_t text_revision, text_session;
static bool text_has_words, ptt_listening, ptt_preempted;
static unsigned ptt_event_order, ptt_down_order, ptt_up_order;
static int64_t s_last_touch_or_key;

static int64_t now_ms(void) { return fake_now; }
static const char *link_via(void) { return s_route; }
static uint32_t link_session(void) { return s_session; }
static void face_lock(void) { assert(!face_locked); face_locked = true; }
static void face_unlock(void) { assert(face_locked); face_locked = false; }
static bool app_games_flush_safe(void) { return progress_flush_safe; }
static void wake(bool sound) { (void)sound; wake_calls++; }
static bool app_lab_active(void) { return false; }
static void audio_tess_gesture(int x, int y) { (void)x; (void)y; gesture_calls++; }
static bool app_games_available(void) {
    return KUBIK_CHARACTER == CHARACTER_TESS && s_online && !ptt_preempted && app_games_flush_safe();
}
static bool tess_games_active(const fake_face_t *face) {
    return face->tess_games.available && face->tess_games.game != TESS_GAME_NONE && !game_card_visible;
}
static bool tess_games_event(fake_face_t *face, int event, float x, float y) {
    assert(face_locked);
    if (event != FEV_TAP) return false;
    face_tap_calls++;
    if (tess_games_active(face) && x == game_target_x && y == game_target_y) {
        game_taps++;
        game_won = true;
        face->tess_games.progress |= face->tess_games.game == TESS_GAME_ECHO ? 1 : 8;
        face->tess_games.game = TESS_GAME_NONE;
        return true;
    }
    return false;
}
esp_err_t factory_reset(void);
static esp_err_t settings_factory_reset(void) {
    if (reset_result == ESP_OK) memset(&g_settings, 0, sizeof g_settings);
    return reset_result;
}
void tess_games_restore(fake_face_t *f, uint8_t progress) {
    assert(face_locked); f->tess_games.progress = progress;
}
void tess_games_set_available(fake_face_t *f, bool available) {
    assert(face_locked);
    f->tess_games.available = available;
    if (!available) f->tess_games.game = TESS_GAME_NONE;
}
static esp_err_t settings_save_tess_progress(uint8_t progress) {
    progress_save_calls++;
    progress_save_mask = progress;
    queue_count_at_save = s_queue.count;
    link_downs_at_save = s_down_calls;
    save_while_face_locked = face_locked;
    if (progress_save_fail) return ESP_FAIL;
    g_settings.tess_progress |= progress;
    return ESP_OK;
}
/* PRODUCTION_TESS_FLUSH */
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
static void handle_link_down(void) {
    s_down_calls++;
    s_online = false;
}
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
static void handle_text_event(const app_ev_t *event) {
    if ((uint32_t)event->a != text_revision || event->session != text_session ||
        (event->session && event->session != s_session) || !text_has_words) return;
    accepted_texts++;
    game_card_visible = true;
}
static void handle_speak_end_event(const app_ev_t *event) { (void)event; }
static void handle_speak_cancel_event(const app_ev_t *event) { (void)event; }
static void handle_error_event(const app_ev_t *event) { (void)event; }
static void handle_set_event(const app_ev_t *event) { (void)event; }
static void handle_agent_capabilities(const app_ev_t *event) { (void)event; }
static bool handle_control_event(const app_ev_t *event) {
    if (event->type == EV_PTT_DOWN) {
        ptt_down_order = ++ptt_event_order;
        ptt_listening = true;
        ptt_preempted = true;
        return true;
    }
    if (event->type == EV_PTT_UP) {
        ptt_up_order = ++ptt_event_order;
        ptt_listening = false;
        ptt_preempted = false;
        return true;
    }
    return false;
}
static bool is_touch_event(uint8_t type) { return type == EV_TAP; }
static void app_touch_event(const app_ev_t *event) {
    if (event->type == EV_TAP) ordinary_tap_calls++;
    if (tess_games_active(&g_face)) {
        face_lock(); tess_games_event(&g_face, FEV_TAP, event->a, event->b); face_unlock();
    }
}
static void handle_touch_event(const app_ev_t *event) {
    touch_events++;
    if (event->a == 99) assert(factory_reset() == reset_result);
    app_touch_event(event);
}
static bool intercept_input(const app_ev_t *event) { (void)event; return false; }
static void app_wake_event(uint32_t value) { (void)value; }
static void app_wake_end_event(int a, int b) { (void)a; (void)b; }
static void app_voice_input_end(int a, int b, uint32_t session) {
    (void)a;
    (void)b;
    (void)session;
}
static void esp_restart(void) {}
/* PRODUCTION_GAME_TAPS */
/* PRODUCTION_POST */
/* PRODUCTION_FACTORY_RESET */
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
    game_won = game_card_visible = false;
    game_target_x = game_target_y = 0;
    game_taps = accepted_texts = ordinary_tap_calls = face_tap_calls = 0;
    text_revision = text_session = 0;
    text_has_words = false;
    ptt_listening = false;
    ptt_preempted = false;
    ptt_event_order = ptt_down_order = ptt_up_order = 0;
    s_last_touch_or_key = 0;
    gesture_calls = wake_calls = 0;
    memset(&g_face, 0, sizeof g_face);
    g_settings.tess_progress = 0;
    fake_now = 60000;
    s_tess_flush_retry_at = 0;
    progress_save_calls = 0; progress_save_mask = 0;
    touch_events = queue_count_at_save = link_downs_at_save = 0;
    progress_save_fail = face_locked = save_while_face_locked = false;
    progress_flush_safe = true;
    reset_result = ESP_OK;
}
static void start_final_game_tap(int x, int y, uint8_t game) {
    s_online = true;
    game_card_visible = false;
    game_won = false;
    game_target_x = x;
    game_target_y = y;
    g_face.tess_games.available = true;
    g_face.tess_games.game = game;
    g_face.tess_games.round = 1;
}
static void post_valid_text(void) {
    text_revision = 7;
    text_session = s_session;
    text_has_words = true;
    app_post_in_session(EV_SRV_TEXT, (int)text_revision, 0, text_session);
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
static void test_progress_flush_is_coalesced_retryable_and_independent_of_queue(void) {
    reset_harness();
    g_face.tess_games.progress = 0x01;
    progress_flush_safe = false;
    app_task_pass();
    assert(progress_save_calls == 0 && g_face.tess_games.progress == 0x01);
    progress_flush_safe = true;
    progress_save_fail = true;
    saturate_queue();
    s_online = true;
    app_post(EV_LINK_DOWN, 0, 0);
    app_task_pass();
    assert(s_queue.count == 24 && progress_save_calls == 1 && progress_save_mask == 0x01);
    assert(touch_events == 8 && queue_count_at_save == 24 && link_downs_at_save == 1);
    assert(g_face.tess_games.progress == 0x01 && g_settings.tess_progress == 0);
    assert(!face_locked && !save_while_face_locked);
    fake_now += 9000;
    g_face.tess_games.progress = 0x03;
    app_task_pass();
    assert(progress_save_calls == 1 && g_face.tess_games.progress == 0x03);
    fake_now += 1000;
    progress_save_fail = false;
    app_task_pass();
    assert(progress_save_calls == 2 && progress_save_mask == 0x03);
    assert(g_settings.tess_progress == 0x03 && !face_locked && !save_while_face_locked);
    app_task_pass();
    assert(progress_save_calls == 2);
}
static void test_factory_reset_cannot_resurrect_progress_before_queued_reboot(void) {
    reset_harness();
    g_face.tess_games.progress = g_settings.tess_progress = 7;
    g_face.tess_games.available = true;
    saturate_queue();
    s_queue.events[7].a = 99; // reset is the last ordinary event before this pass's flush
    app_task_pass();
    assert(s_queue.count == 25); // 24 old events plus the delayed reboot
    assert(!progress_save_calls && !g_settings.tess_progress && !g_face.tess_games.progress);
    assert(!g_face.tess_games.available && !face_locked);
    while (s_queue.count) app_task_pass();
    assert(!progress_save_calls && !g_settings.tess_progress);

    reset_harness(); reset_result = ESP_FAIL;
    g_face.tess_games.progress = g_settings.tess_progress = 7;
    saturate_queue(); s_queue.events[7].a = 99;
    app_task_pass();
    assert(s_queue.count == 24 && g_face.tess_games.progress == 7 && g_settings.tess_progress == 7);
}

static void post_game_preemption(app_ev_type_t type) {
    if (type == EV_SRV_TEXT) post_valid_text();
    else app_post(EV_LINK_DOWN, 0, 0);
}
static void assert_game_tap_suppressed(void) {
    assert(!game_won && !game_taps && !face_tap_calls && !ordinary_tap_calls);
    assert(!g_face.tess_games.progress && !g_settings.tess_progress);
    assert(!gesture_calls && !wake_calls && !s_last_touch_or_key);
}

static void test_game_taps_defer_across_priority_events(void) {
    const app_ev_type_t preemptions[] = {EV_SRV_TEXT, EV_LINK_DOWN};
    for (unsigned i = 0; i < sizeof preemptions / sizeof preemptions[0]; i++) {
        for (unsigned event_first = 0; event_first < 2; event_first++) {
            reset_harness();
            start_final_game_tap(i == 0 ? 240 : 300, i == 0 ? 240 : 260,
                                 i == 0 ? TESS_GAME_ECHO : TESS_GAME_CATCH);
            if (event_first) post_game_preemption(preemptions[i]);
            app_post(EV_TAP, game_target_x, game_target_y);
            if (!event_first) post_game_preemption(preemptions[i]);
            app_task_pass();

            assert(preemptions[i] == EV_SRV_TEXT ? accepted_texts == 1 : s_down_calls == 1);
            assert_game_tap_suppressed();
        }
    }
}

static void assert_game_tap_applied(void) {
    assert(game_won && game_taps == 1 && face_tap_calls == 1 && !ordinary_tap_calls);
    assert(gesture_calls == 1 && wake_calls == 1 && s_last_touch_or_key == fake_now);
    assert(!accepted_texts && !s_down_calls);
}

typedef struct {
    bool text, words;
    uint32_t current_revision, posted_revision, session;
    const char *route;
} unrelated_event_t;

static void post_unrelated_event(const unrelated_event_t *event) {
    if (event->text) {
        text_revision = event->current_revision;
        text_session = s_session;
        text_has_words = event->words;
        app_post_in_session(EV_SRV_TEXT, (int)event->posted_revision, 0, event->session);
    } else if (event->route) {
        s_route = event->route;
        app_post(EV_LINK_DOWN, 0, 0);
    }
}

static void test_only_effective_preemption_cancels_game_taps(void) {
    static const unrelated_event_t cases[] = {
        {0},
        {.text = true, .words = true, .current_revision = 7, .posted_revision = 7, .session = 41},
        {.text = true, .words = true, .current_revision = 8, .posted_revision = 7, .session = 42},
        {.text = true, .current_revision = 7, .posted_revision = 7, .session = 42},
        {.route = "wifi"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_harness();
        start_final_game_tap(240, 240, TESS_GAME_ECHO);
        post_unrelated_event(&cases[i]);
        app_post(EV_TAP, game_target_x, game_target_y);
        app_task_pass();
        assert_game_tap_applied();
    }
}

static void test_game_tap_stays_canceled_after_recovery(void) {
    reset_harness();
    start_final_game_tap(300, 260, TESS_GAME_CATCH);
    app_post(EV_TAP, game_target_x, game_target_y);
    app_post(EV_LINK_DOWN, 0, 0);
    app_post_in_session(EV_SRV_WELCOME, 70, 1, s_session);
    app_task_pass();
    assert(s_down_calls == 1 && s_online && !game_won && !game_taps && !ordinary_tap_calls);
}

static void test_ptt_and_round_fences_suppress_deferred_hits(void) {
    reset_harness();
    app_post(EV_PTT_DOWN, 0, 0); app_post(EV_PTT_UP, 0, 0);
    app_task_pass();
    assert(ptt_down_order == 1 && ptt_up_order == 2 && !ptt_listening);

    reset_harness();
    start_final_game_tap(240, 240, TESS_GAME_ECHO);
    app_post(EV_TAP, game_target_x, game_target_y);
    app_post(EV_PTT_DOWN, 0, 0); app_post(EV_PTT_UP, 0, 0);
    app_task_pass();
    assert(ptt_down_order == 1 && ptt_up_order == 2 && !game_won && !game_taps && !ordinary_tap_calls);

    reset_harness();
    start_final_game_tap(240, 240, TESS_GAME_ECHO);
    app_ev_t tap = {.type = EV_TAP, .a = 240, .b = 240};
    game_tap_t deferred;
    assert(capture_game_tap(&tap, &deferred) && deferred.round == 1);
    g_face.tess_games.round++;
    apply_game_tap(&deferred);
    assert(!game_won && !game_taps && !ordinary_tap_calls && !gesture_calls && !wake_calls);
}

int main(void) {
    test_disconnect_survives_full_queue();
    test_replacement_route_ignores_queued_disconnect();
    test_welcome_survives_full_queue_and_keeps_session_fence();
    test_reserved_slots_keep_key_speech_order_and_coalesce();
    test_progress_flush_is_coalesced_retryable_and_independent_of_queue();
    test_factory_reset_cannot_resurrect_progress_before_queued_reboot();
    test_game_taps_defer_across_priority_events();
    test_only_effective_preemption_cancels_game_taps();
    test_game_tap_stays_canceled_after_recovery();
    test_ptt_and_round_fences_suppress_deferred_hits();
    puts("app-post: reserved delivery, key/speech order and retryable local-progress flush passed");
}
