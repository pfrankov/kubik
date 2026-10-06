#include "app_speech.h"
#include <stdio.h>
#include "audio.h"
#include "app_wake.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "heap_probe.h"
#include "link.h"
#include "tls_mem.h"
#include <string.h>

atomic_int s_gen = -1;
static SemaphoreHandle_t s_lock;
static uint32_t s_session;
static bool s_ended;
static int64_t s_started_us, s_progress_ms;
static int64_t s_rx_us;
static uint32_t s_rx_packets, s_rx_gap_ms, s_rx_first_ms;

void app_speech_init(void) {
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_lock ? ESP_OK : ESP_ERR_NO_MEM);
}

static bool matches(int gen, uint32_t session) {
    return gen >= 0 && gen == s_gen && session && session == s_session && session == link_session();
}

void app_speech_begin(int gen, uint32_t session) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_gen = gen;
    s_session = session;
    s_ended = false;
    s_started_us = esp_timer_get_time();
    s_rx_us = 0; s_rx_packets = s_rx_gap_ms = s_rx_first_ms = 0;
    s_progress_ms = s_started_us / 1000;
    app_wake_suspend();
    tls_mem_speech(!strcmp(link_via(), "wifi"));
    audio_stream_begin();
    xSemaphoreGive(s_lock);
}

bool app_speech_matches(int gen, uint32_t session) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool current = matches(gen, session);
    xSemaphoreGive(s_lock);
    return current;
}

void app_speech_write(int gen, uint32_t session, const uint8_t *frame, size_t bytes) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (matches(gen, session) && audio_stream_write_ima(frame, bytes)) {
        int64_t now = esp_timer_get_time();
        uint32_t gap = (uint32_t)((now - s_rx_us) / 1000);
        if (!s_rx_packets) s_rx_first_ms = (uint32_t)((now - s_started_us) / 1000);
        else if (gap > s_rx_gap_ms) s_rx_gap_ms = gap;
        s_rx_us = now; ++s_rx_packets;
    }
    xSemaphoreGive(s_lock);
}

bool app_speech_end(int gen, uint32_t session) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool applied = matches(gen, session);
    if (applied) { s_ended = true; audio_stream_end(); }
    xSemaphoreGive(s_lock);
    return applied;
}

static void clear_locked(void) {
    tls_mem_speech(false);
    s_gen = -1;
    s_session = 0;
    s_ended = false;
}

static uint32_t log_playback(const char *reason, int gen) {
    uint32_t gaps, gap_ms, mix_us, write_us, played_ms = audio_stream_played_ms();
    audio_stream_gaps(&gaps, &gap_ms);
    audio_stream_timing(&mix_us, &write_us);
    ESP_LOGI("app", "speech %s gen=%d ms=%u elapsed=%u gaps=%u (%u ms) mix<=%uus dma-wait<=%uus via=%s", reason, gen,
             (unsigned)played_ms, (unsigned)((esp_timer_get_time() - s_started_us) / 1000), (unsigned)gaps,
             (unsigned)gap_ms, (unsigned)mix_us, (unsigned)write_us, link_via());
    ESP_LOGI("app", "speech rx gen=%d packets=%u first=%ums gap<=%ums tail=%ums", gen, (unsigned)s_rx_packets,
             (unsigned)s_rx_first_ms, (unsigned)s_rx_gap_ms,
             s_rx_packets ? (unsigned)((esp_timer_get_time() - s_rx_us) / 1000) : 0);
    return played_ms;
}

void app_speech_stop(bool tell_server) {
    char message[64];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int gen = s_gen;
    uint32_t session = s_session;
    if (gen >= 0) log_playback("stopped", gen);
    clear_locked();
    audio_stream_stop();
    xSemaphoreGive(s_lock);
    if (tell_server && gen >= 0) {
        snprintf(message, sizeof message, "{\"t\":\"cancel\",\"gen\":%d}", gen);
        link_send_json_in_session(message, session);
    }
}

bool app_speech_cancel(int gen, uint32_t session) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool applied = matches(gen, session);
    if (applied) {
        log_playback("cancelled", gen); clear_locked(); audio_stream_stop();
    }
    xSemaphoreGive(s_lock);
    // ACK also covers an already-drained gen; foreign sessions never gain an ACK.
    if (session && session == link_session()) {
        char message[48]; snprintf(message, sizeof message, "{\"t\":\"cancelled\",\"gen\":%d}", gen);
        link_send_json_in_session(message, session);
    }
    return applied;
}

bool app_speech_tick(int64_t now_ms, bool progress) {
    char message[96] = "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int gen = s_gen;
    uint32_t session = s_session;
    bool completed = false;
    if (gen >= 0 && !matches(gen, session)) {
        log_playback("lost", gen);
        clear_locked(); // independent of delivery of the ordinary LINK_DOWN event
        audio_stream_stop();
    } else if (gen >= 0 && s_ended && audio_stream_drained()) {
        uint32_t played_ms = log_playback("played", gen);
        if (played_ms) snprintf(message, sizeof message, "{\"t\":\"played\",\"gen\":%d,\"ms\":%u}", gen, (unsigned)played_ms);
        audio_stream_stop(); // drained streams must release the active flag so dark codecs can doze
        clear_locked();
        completed = true;
    } else if (gen >= 0 && progress && !s_ended && now_ms - s_progress_ms >= 100) {
        s_progress_ms = now_ms;
        snprintf(message, sizeof message, "{\"t\":\"progress\",\"gen\":%d,\"ms\":%u}", gen, (unsigned)audio_stream_played_ms());
    }
    xSemaphoreGive(s_lock);
    // Never hold playback's mutex while acquiring the route mutex (RX holds them in the opposite order).
    if (message[0]) link_send_json_in_session(message, session);
    if (completed) hp_mark("speech played");
    return completed;
}
