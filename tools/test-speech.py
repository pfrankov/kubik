#!/usr/bin/env python3
"""Actual speech callbacks, mailbox and playback coordinator across colliding reconnect generations."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "firmware/main"
IDF = Path(os.environ.get("IDF_PATH", str(Path.home() / "esp/esp-idf-v5.5.1")))


def body(name):
    return "\n".join(line for line in (MAIN / name).read_text().splitlines() if not line.startswith("#include"))


protocol = body("app_protocol.c")
callbacks = protocol[protocol.index("static void handle_speak_json"):protocol.index("static void handle_error_json")]
callbacks += protocol[protocol.index("static void on_audio"):protocol.index("static void on_pair")]
events = body("app_events.c")
callbacks += events[events.index("static void handle_speak_end_event"):events.index("static void handle_error_event")]
callbacks += events[events.index("static void handle_speak_event"):events.index("static void handle_text_event")]
callbacks += events[events.index("static bool handle_link_event"):events.index("static void handle_cron_event")]
mock = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "app_speech.h"
#include "app_mailbox.h"
#define ESP_OK 0
#define ESP_ERR_NO_MEM -1
#define ESP_ERROR_CHECK(x) assert((x) == 0)
static char diagnostic[512];
#define ESP_LOGI(tag, ...) ((void)snprintf(diagnostic, sizeof diagnostic, __VA_ARGS__))
#define portMAX_DELAY 0
typedef int SemaphoreHandle_t;
static bool held, ended, change_before_send;
static unsigned bytes, sent, begins, stops;
static unsigned wake_suspends;
static bool active;
static uint32_t current = 1;
static char last[128];
static app_mailbox_t inbox;
enum { EV_SRV_SPEAK, EV_SRV_SPEAK_END, EV_LINK_UP, EV_LINK_DOWN, EV_PAIR };
enum { TE_RESOLVE, SFX_NOTIFY, FEV_NOTIFY, FEV_TALK_START };
static unsigned ui_calls, down_calls;
static int64_t s_last_activity;
static const char *via = "usb";
static int xSemaphoreCreateMutex(void) { return 1; }
static void xSemaphoreTake(int lock, int timeout) { (void)lock; (void)timeout; assert(!held); held = true; }
static void xSemaphoreGive(int lock) { (void)lock; assert(held); held = false; }
static int64_t esp_timer_get_time(void) { return 2000000; }
static uint32_t link_session(void) { return current; }
static const char *link_via(void) { return via; }
static bool link_send_json_in_session(const char *json, uint32_t session) {
    assert(!held); // RX owns route then playback; outbound may not hold playback then acquire route
    if (change_before_send) { current++; change_before_send = false; }
    if (session != current) return false;
    sent++; snprintf(last, sizeof last, "%s", json); return true;
}
static void hp_mark(const char *name) { (void)name; }
static void tls_mem_speech(bool active) { (void)active; }
static void app_wake_suspend(void) { assert(held); wake_suspends++; }
static void audio_stream_begin(void) { assert(held && wake_suspends == begins + 1); active = true; ended = false; bytes = 0; begins++; }
static void audio_stream_end(void) { assert(held); ended = true; }
static void audio_stream_stop(void) { assert(held); active = false; ended = true; bytes = 0; stops++; }
static unsigned audio_stream_write_ima(const uint8_t *p, size_t len) {
    (void)p; if (ended) { return 0; } bytes += (unsigned)len; return (unsigned)len;
}
static bool tail_pending;
static bool audio_stream_drained(void) { return ended && !tail_pending; }
static uint32_t audio_stream_played_ms(void) { return bytes / 12; }
static void audio_stream_gaps(uint32_t *gaps, uint32_t *ms) { *gaps = *ms = 0; }
static void audio_stream_timing(uint32_t *mix, uint32_t *write) { *mix = *write = 0; }
static void app_post_in_session(int type, int a, int b, uint32_t session) {
    app_mailbox_put(&inbox, (unsigned)type, (app_ev_t){.type = (uint8_t)type, .a = a, .b = b, .session = session});
}
static void talk_fire(int event) { (void)event; ui_calls++; }
static unsigned reply_wakes;
static void wake_for_reply(void) { reply_wakes++; }
static void audio_sfx(int sound) { (void)sound; ui_calls++; }
static void face_ev(int event, int a, int b) { (void)event; (void)a; (void)b; ui_calls++; }
static int64_t now_ms(void) { return 2000; }
static void handle_link_down(void) { down_calls++; app_speech_stop(false); }
static void handle_pair_event(void) {}
static void app_journal_event(const app_ev_t *e) { (void)e; }
'''
test = r'''
static void speak(void) {
    cJSON *j = cJSON_Parse("{\"t\":\"speak\",\"gen\":7}");
    handle_speak_json(j); cJSON_Delete(j);
}
static void end(void) {
    cJSON *j = cJSON_Parse("{\"t\":\"speak_end\",\"gen\":7}");
    handle_speak_end_json(j); cJSON_Delete(j);
}
int main(void) {
    app_speech_init(); speak();
    app_ev_t event; assert(app_mailbox_take(&inbox, &event));
    app_ev_t old_speak = event;
    end(); // END remains pending while USB RX (priority 8) reads the replacement session's SPEAK
    current++; speak();
    assert(app_mailbox_take(&inbox, &event) && event.type == EV_SRV_SPEAK_END && event.session == 1);
    handle_speak_end_event(&event);
    assert(!ended && !app_speech_tick(2200, false));
    handle_speak_event(&old_speak); assert(ui_calls == 0 && reply_wakes == 0);
    static uint8_t pcm[48000]; on_audio(3, 7, pcm, sizeof pcm);
    assert(bytes == sizeof pcm); // a stale END did not reject the replacement stream's audio
    on_audio(3, 6, pcm, sizeof pcm); assert(bytes == sizeof pcm);
    assert(app_mailbox_take(&inbox, &event) && event.type == EV_SRV_SPEAK && event.session == current);
    assert(app_speech_matches(event.a, event.session));
    handle_speak_event(&event); assert(ui_calls == 2 && reply_wakes == 1 && s_last_activity == 2000);
    end(); assert(app_mailbox_take(&inbox, &event)); handle_speak_end_event(&event);
    tail_pending = true;
    assert(!app_speech_tick(2300, true) && sent == 0 && s_gen == 7 && active);
    tail_pending = false;
    assert(app_speech_tick(2300, true) && sent == 1 && strstr(last, "\"played\"") && strstr(last, "4000"));
    assert(strstr(diagnostic, "packets=1") && strstr(diagnostic, "gap<=0ms"));
    assert(s_gen == -1 && !active); // completed playback releases codec doze eligibility
    speak(); on_audio(3, 7, pcm, sizeof pcm); app_speech_end(7, current); change_before_send = true;
    assert(app_speech_tick(2400, false) && sent == 1); // route changes between completion and ACK
    speak(); assert(!app_speech_tick(2099, true) && sent == 1);
    assert(!app_speech_tick(2100, true) && sent == 2 && strstr(last, "progress"));
    assert(!app_speech_tick(2199, true) && sent == 2); // bounded feedback, not one write per UI frame
    assert(!app_speech_tick(2200, true) && sent == 3);
    change_before_send = true; app_speech_stop(true); assert(sent == 3 && s_gen == -1);
    speak(); unsigned old_stops = stops; current++; // LINK_DOWN may be dropped from the full ordinary queue
    assert(!app_speech_tick(3000, true) && s_gen == -1 && stops == old_stops + 1 && sent == 3);
    speak(); app_speech_stop(true); assert(sent == 4 && strstr(last, "cancel"));
    speak(); app_speech_end(7, current); // allocation refusal leaves no played samples
    assert(app_speech_tick(4000, false) && sent == 4 && s_gen == -1 && !active);
    assert(strstr(diagnostic, "packets=0") && strstr(diagnostic, "tail=0ms"));
    speak(); app_ev_t down = {.type = EV_LINK_DOWN};
    assert(handle_link_event(&down) && down_calls == 0 && s_gen == 7);
    via = "none"; assert(handle_link_event(&down) && down_calls == 1 && s_gen == -1);
    assert(begins == 8);
    puts("speech: colliding generation/stale END, origin-bound ACK/progress/cancel, lost disconnect exact");
}
'''
with tempfile.TemporaryDirectory(prefix="kubik-speech-") as tmp:
    source = Path(tmp) / "speech.c"
    source.write_text(mock + body("app_mailbox.c") + body("app_speech.c") + callbacks + test)
    exe = Path(tmp) / "speech"
    cjson_object = Path(tmp) / "cjson.o"
    subprocess.run([os.environ.get("CC", "cc"), "-O1", "-g", "-fsanitize=address,undefined",
                    "-Wno-deprecated-declarations", "-c", str(IDF / "components/json/cJSON/cJSON.c"),
                    "-o", str(cjson_object)], check=True)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
                    f"-I{MAIN}", f"-I{IDF}/components/json/cJSON", str(source),
                    str(cjson_object), "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
