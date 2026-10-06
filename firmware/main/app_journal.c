#include "app_journal.h"
#include "app_internal.h"
#include <stdio.h>

static void add(journal_kind_t kind, const char *text, const char *reaction) {
    face_lock();
    event_journal_add(&g_face.journal, (uint32_t)(now_ms() / 1000), kind, text, reaction);
    face_unlock();
}
void app_journal_text(const char *text, bool notification) {
    event_journal_add(&g_face.journal, (uint32_t)(now_ms() / 1000), notification ? JOURNAL_NOTICE : JOURNAL_REPLY,
                      text, notification ? "Notification / text card" : "Reply / text card");
}
static void state(int state) {
    static const char *const names[] = {"Idle", "Listening", "Transcribing", "Thinking", "Speaking"};
    static const char *const reactions[] = {"Rest after pending work finishes", "Pose follows microphone capture", "Waiting for recognition", "Waiting for agent", "Pose follows audible playback"};
    if (state >= 0 && state <= SS_SPEAKING) add(JOURNAL_STATE, names[state], reactions[state]);
}
static void activity(int own, int other) {
    if (own < 0 || own >= ACT_COUNT || other < 0 || other >= ACT_COUNT) return;
    char text[96];
    snprintf(text, sizeof text, "Here: %s / elsewhere: %s", own ? app_activity_names[own] : "idle", other ? app_activity_names[other] : "idle");
    add(JOURNAL_ACTIVITY, text, own ? "Waiting pose / activity indicator" : other ? "Background activity indicator" : "Activity indicator cleared");
}
static void emotion(int value) {
    static const char *const names[EMO_COUNT] = {"Neutral", "Happy", "Joy", "Love", "Sad", "Angry", "Surprised", "Confused", "Sleepy", "Thinking", "Wink", "Shy", "Proud", "Dizzy"};
    if (value >= 0 && value < EMO_COUNT) add(JOURNAL_EMOTION, names[value], "Character emotion reaction");
}
static void error(int value) {
    static const char *const names[] = {"No speech recognized", "Recognition failed", "Agent failed", "Voice failed", "Agent busy", "Not authorized", "Agent error"};
    const char *text = value >= 0 && value <= ERR_OTHER ? names[value] : "Agent error";
    add(JOURNAL_ERROR, text, value == ERR_BUSY ? "Thinking / busy indicator" : value == ERR_STT_EMPTY ? "Not-heard reaction" : "Error indicator / failure reaction");
}
static bool scheduler_changed(const app_ev_t *e) {
    static uint32_t previous_session;
    static int running = -1;
    static int64_t due = -1;
    uint32_t session = link_session();
    int64_t next = e->b < 0 ? -1 : now_ms() + (int64_t)e->b * 1000;
    int64_t drift = next - due;
    bool changed = session != previous_session || running != e->a || (next < 0) != (due < 0) || drift > 2500 || drift < -2500;
    previous_session = session; running = e->a; due = next;
    return changed;
}
static void metadata(const app_ev_t *e) {
    char text[96];
    switch (e->type) {
    case EV_SRV_CRON:
        if (!scheduler_changed(e)) return;
        if (e->b >= 0) snprintf(text, sizeof text, "%d running / reminder in %d s", (int)e->a, (int)e->b);
        else snprintf(text, sizeof text, "%d running / no reminder", (int)e->a);
        add(JOURNAL_ACTIVITY, text, e->a ? "Rotating clock / no forced wake" : e->b >= 0 ? "Reminder clock / no forced wake" : "Clock cleared"); break;
    case EV_SRV_SET:
        snprintf(text, sizeof text, "Speech %d / brightness %d", (int)e->a, (int)e->b);
        add(JOURNAL_SETTINGS, text, "Requested levels applied"); break;
    default: break;
    }
}
void app_journal_event(const app_ev_t *e) {
    switch (e->type) {
    case EV_SRV_WELCOME: add(JOURNAL_LINK, "Agent connected", "Ready / connection greeting"); break;
    case EV_LINK_DOWN: add(JOURNAL_LINK, "Agent disconnected", "Offline after connection grace period"); break;
    case EV_SRV_STATE: state(e->a); break;
    case EV_SRV_ACTIVITY: activity((int)e->a, (int)e->b); break;
    case EV_SRV_EMOTION: emotion(e->a); break;
    case EV_SRV_ERROR: error(e->a); break;
    case EV_SRV_SPEAK: add(JOURNAL_SPEECH, e->b ? "Notification voice started" : "Reply voice started", "Pose follows audible playback"); break;
    case EV_SRV_SPEAK_END: add(JOURNAL_SPEECH, "Voice stream ended", "Finish buffered audio, then return"); break;
    case EV_SRV_SPEAK_CANCEL: add(JOURNAL_SPEECH, "Voice interrupted", "Playback cleared"); break;
    default: metadata(e); break;
    }
}

void app_journal_info(cJSON *info) {
    face_lock();
    const event_journal_t *j = &g_face.journal;
    cJSON_AddBoolToObject(info, "menu_ready", g_face.menu.open && g_face.menu.k >= 1.f);
    cJSON_AddBoolToObject(info, "event_overlay", j->overlay);
    cJSON_AddBoolToObject(info, "events_open", j->open);
    cJSON_AddBoolToObject(info, "events_detail", j->detail);
    cJSON_AddBoolToObject(info, "events_expired", j->detail_expired);
    cJSON_AddNumberToObject(info, "events_count", j->count);
    cJSON_AddNumberToObject(info, "events_offset", j->offset);
    face_unlock();
}
