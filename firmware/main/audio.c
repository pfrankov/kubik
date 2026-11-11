#include "audio.h"
#include "audio_ring.h"
#include "task_placement.h"
#include "audio_levels.h"
#include "audio_signal.h"
#include "audio_sfx.h"
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mic_capture.h"
#include "mic_capture_task.h"
#include "mic_delivery.h"
#include "audio_board.h"
#include "audio_output_tail.h"
#include "audio_output_profile.h"
static const char *TAG = "audio";
#define SPK_FRAME AUDIO_SPEAKER_FRAMES  // 20 ms
#include "audio_target.h"
#define MIC_CHUNK 240  // frames per read (10 ms); MIC_FRAME_SAMPLES is a whole number of them
static bool s_duplex;
static esp_codec_dev_handle_t s_out;
static volatile bool s_speech_audible;
bool audio_stream_playing(void) { return s_speech_audible; }
static volatile float s_spk_level, s_mic_level;
float audio_spk_level(void) { return s_spk_level; }
float audio_mic_level(void) { return s_mic_level; }
// Keep the output configuration and DMA buffers through doze; the ADC/RX are off whenever PTT is closed.
static const audio_codec_if_t *s_out_codec;
static i2s_chan_handle_t s_tx;
static volatile bool s_doze, s_codecs_down, s_gate, s_mic_closing, s_mic_transition;
static portMUX_TYPE s_audio_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_spk_task, s_mic_task;
static mic_capture_t s_capture;
static SemaphoreHandle_t s_mic_read_lock;
void audio_kick(void) {
    if (s_spk_task) xTaskNotifyGive(s_spk_task);
}
void audio_doze(bool doze) {
    portENTER_CRITICAL(&s_audio_mux);
    bool changed = doze != s_doze;
    s_doze = doze;
    portEXIT_CRITICAL(&s_audio_mux);
    if (!changed) return;
    if (!doze) audio_kick();
}
static bool may_doze(void) {
    portENTER_CRITICAL(&s_audio_mux);
    bool quiet = s_doze && !audio_ring_active() && !s_gate && !s_mic_transition && !s_capture.rx_enabled && !s_capture.adc_enabled;
    portEXIT_CRITICAL(&s_audio_mux);
    return quiet && !audio_sfx_playing() && !audio_self_audible() &&
        audio_ring_output_idle();
}
static void doze_while_quiet(void) {
    if (!may_doze()) return;
    portENTER_CRITICAL(&s_audio_mux);
    bool reserved = s_doze && !audio_ring_active() && !s_gate && !s_mic_transition && !s_capture.rx_enabled && !s_capture.adc_enabled;
    if (reserved) s_codecs_down = true;
    portEXIT_CRITICAL(&s_audio_mux);
    if (!reserved) return;
    int64_t t0 = esp_timer_get_time();
    ESP_ERROR_CHECK(s_out_codec->enable(s_out_codec, false));
    ESP_ERROR_CHECK(i2s_channel_disable(s_tx));
    s_speech_audible = false; s_spk_level = 0.f; // no stale speech/work flag while output sleeps
    ESP_LOGI(TAG, "codecs dozing");
    while (may_doze()) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(s_out_codec->enable(s_out_codec, true));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_out, 100));
    portENTER_CRITICAL(&s_audio_mux);
    s_codecs_down = false;
    portEXIT_CRITICAL(&s_audio_mux);
    xTaskNotifyGive(s_mic_task);
    ESP_LOGI(TAG, "codecs awake after %lld s", (esp_timer_get_time() - t0) / 1000000);
}
static bool mix_speaker_frame(int16_t *samples, int32_t *mix, bool have_speech,
                              float level_history[4], int *history_index) {
    for (int i = 0; i < SPK_FRAME; i++) samples[i] = audio_levels_speech(mix[i]);
    float level = have_speech ? audio_signal_level(samples, SPK_FRAME, -48.f, 40.f) : 0.f;
    memset(mix, 0, SPK_FRAME * sizeof(*mix));
    audio_sfx_mix(mix, SPK_FRAME);
    bool ui_audible = false, audible = false;
    for (int i = 0; i < SPK_FRAME; i++) {
        int32_t ui = audio_levels_interface(mix[i]);
        ui_audible |= ui != 0;
        int32_t value = audio_levels_mix(samples[i] + ui);
        samples[i] = value > 32767 ? 32767 : value < -32768 ? -32768 : (int16_t)value;
        audible |= samples[i] != 0;
    }
    audio_sfx_note_output(ui_audible);
    s_spk_level = level_history[*history_index];
    level_history[*history_index] = level;
    *history_index = (*history_index + 1) & 3;
    s_speech_audible = have_speech && audio_levels_speech_enabled();
    return audible;
}
static void speaker_cycle(float level_history[4], int *history_index) {
    static int16_t samples[SPK_FRAME];
    static int32_t mix[SPK_FRAME];
    uint32_t epoch = audio_ring_epoch();
    int64_t started = esp_timer_get_time();
    memset(mix, 0, sizeof mix);
    bool have_speech = audio_ring_read(samples, mix);
    audio_ring_release_idle();
    bool audible = mix_speaker_frame(samples, mix, have_speech, level_history, history_index);
    int64_t mixed = esp_timer_get_time();
    audio_ring_note_mix(epoch, (uint32_t)(mixed - started));
    int error = esp_codec_dev_write(s_out, samples, sizeof samples);
    audio_output_profile_note(esp_timer_get_time(), (uint32_t)(mixed - started), audible, error != ESP_OK);
    if (have_speech) audio_ring_output_written();
    uint32_t waited = (uint32_t)(esp_timer_get_time() - mixed);
    audio_ring_note_write(epoch, waited);
}
static void speaker_task(void *arg) {
    float level_history[4] = {0};
    int history_index = 0;
    while (1) {
        doze_while_quiet();
        speaker_cycle(level_history, &history_index);
    }
}
static mic_frame_fn s_mic_fn;
static bool s_wake_capture;
static uint32_t s_capture_epoch;
static bool capture_requested(void) {
    portENTER_CRITICAL(&s_audio_mux);
    bool requested = s_gate && !s_mic_closing && !s_codecs_down;
    portEXIT_CRITICAL(&s_audio_mux);
    return requested;
}
static bool sync_capture(bool requested) {
    portENTER_CRITICAL(&s_audio_mux);
    requested = requested && s_gate && !s_mic_closing && !s_codecs_down;
    s_mic_transition = true;
    portEXIT_CRITICAL(&s_audio_mux);
    bool ready = mic_capture_sync(&s_capture, requested);
    portENTER_CRITICAL(&s_audio_mux);
    s_mic_transition = false;
    portEXIT_CRITICAL(&s_audio_mux);
    return ready;
}
static bool capture_stopped(void) { return !s_capture.rx_enabled && !s_capture.adc_enabled; }
static bool capture_done(bool open) { return open ? s_capture.ready : capture_stopped(); }
static void wait_capture(bool open) {
    int64_t deadline = esp_timer_get_time() + 500000;
    while (!capture_done(open) && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(1));
    if (capture_done(open)) ESP_LOGI(TAG, "microphone %s, reads=%lu idle=%lu", open ? "enabled" : "disabled",
                                      (unsigned long)s_capture.reads, (unsigned long)s_capture.idle_reads);
    else ESP_LOGE(TAG, "microphone %s timed out, rx=%d adc=%d", open ? "enable" : "disable",
                  s_capture.rx_enabled, s_capture.adc_enabled);
}
void audio_mic_status(audio_mic_status_t *status) {
    if (!status) return;
    portENTER_CRITICAL(&s_audio_mux);
    status->dozing = s_codecs_down;
    status->open = s_gate && !s_mic_closing && !s_wake_capture;
    status->enabled = s_capture.adc_enabled;
    status->rx_enabled = s_capture.rx_enabled;
    status->adc_enabled = s_capture.adc_enabled;
    status->reads = s_capture.reads;
    status->idle_reads = s_capture.idle_reads;
    portEXIT_CRITICAL(&s_audio_mux);
}
static void finish_capture_gate(bool open, bool local_wake) {
    // Only DMA reads preempt rendering; queued TLS/VAD runs at 7, wake inference at 6.
    if (s_mic_task) { vTaskPrioritySet(s_mic_task, open && !local_wake ? 9 : 6); xTaskNotifyGive(s_mic_task); }
    if (open) audio_kick();
    wait_capture(open);
    if (!open && capture_stopped()) {
        portENTER_CRITICAL(&s_audio_mux);
        s_mic_fn = NULL;
        portEXIT_CRITICAL(&s_audio_mux);
    }
}
static void deliver_queued(void *context, const int16_t *frame, const uint8_t *ima, uint32_t epoch);
static bool capture_gate(bool open, mic_frame_fn fn, bool local_wake, const uint32_t *expected) {
    if (open) audio_sfx_quiet();
    if (!open) {
        portENTER_CRITICAL(&s_audio_mux);
        s_mic_closing = true;
        s_mic_level = 0;
        portEXIT_CRITICAL(&s_audio_mux);
        xSemaphoreTake(s_mic_read_lock, portMAX_DELAY);
        xSemaphoreGive(s_mic_read_lock);
        mic_delivery_stop();
    }
    xSemaphoreTake(s_mic_read_lock, portMAX_DELAY);
    portENTER_CRITICAL(&s_audio_mux);
    if (expected && *expected != s_capture_epoch) {
        portEXIT_CRITICAL(&s_audio_mux); xSemaphoreGive(s_mic_read_lock); return false;
    }
    portEXIT_CRITICAL(&s_audio_mux);
    if (open && !local_wake && !mic_delivery_start(deliver_queued, NULL, s_duplex && !local_wake)) {
        xSemaphoreGive(s_mic_read_lock); return false;
    }
    portENTER_CRITICAL(&s_audio_mux);
    if (expected && *expected != s_capture_epoch) {
        portEXIT_CRITICAL(&s_audio_mux); xSemaphoreGive(s_mic_read_lock); mic_delivery_stop(); return false;
    }
    ++s_capture_epoch;
    s_wake_capture = open && local_wake;
    if (open) s_mic_fn = fn;
    s_gate = open;
    s_mic_closing = false;
    portEXIT_CRITICAL(&s_audio_mux);
    xSemaphoreGive(s_mic_read_lock);
    finish_capture_gate(open, local_wake);
    return capture_done(open);
}
// RX handlers hold the route lock: invalidate capture without waiting for a
// callback that may be sending. The app subsequently drains it before freeing VAD.
void audio_mic_request_stop(void) {
    audio_mic_request_stop_epoch(0);
}
bool audio_mic_request_stop_epoch(uint32_t epoch) {
    portENTER_CRITICAL(&s_audio_mux);
    if (epoch && epoch != s_capture_epoch) { portEXIT_CRITICAL(&s_audio_mux); return false; }
    s_gate = false; s_mic_closing = true; ++s_capture_epoch; s_mic_level = 0;
    portEXIT_CRITICAL(&s_audio_mux);
    if (s_mic_task) xTaskNotifyGive(s_mic_task);
    return true;
}
bool audio_mic_start_epoch(mic_frame_fn fn, uint32_t epoch) { return capture_gate(true, fn, false, &epoch); }
void audio_mic_set_duplex(bool enabled) { s_duplex = enabled; }
bool audio_mic_gate(bool open, mic_frame_fn fn) { return capture_gate(open, fn, false, NULL); }
bool audio_wake_gate(bool open, mic_frame_fn fn) {
    if (open || s_wake_capture) capture_gate(open, fn, true, NULL);
    return open ? s_capture.ready && s_wake_capture : capture_stopped();
}
uint32_t audio_capture_epoch(void) {
    portENTER_CRITICAL(&s_audio_mux); uint32_t epoch = s_capture_epoch; portEXIT_CRITICAL(&s_audio_mux); return epoch;
}
bool audio_mic_is_open(void) { return s_gate && !s_mic_closing && !s_wake_capture; }
static void prepare_voice(int16_t *buf, int32_t *dc) {
    mic_capture_center(buf, MIC_FRAME_SAMPLES, dc);
    s_mic_level = s_wake_capture ? 0.f : audio_signal_level(buf, MIC_FRAME_SAMPLES, -52.f, 38.f);
    if (!s_duplex && audio_self_audible()) memset(buf, 0, MIC_FRAME_SAMPLES * sizeof buf[0]);
}
static bool task_requested(void *context) { (void)context; return capture_requested(); }
static bool task_sync(void *context, bool requested) { (void)context; return sync_capture(requested); }
static bool task_read(void *context, bool permitted, void *buffer, size_t bytes, uint32_t timeout_ms) {
    if (!permitted || xSemaphoreTake(s_mic_read_lock, portMAX_DELAY) != pdTRUE) return false;
    portENTER_CRITICAL(&s_audio_mux);
    bool allowed = s_gate && !s_mic_closing && !s_codecs_down && s_capture.ready;
    if (allowed) s_mic_transition = true;
    portEXIT_CRITICAL(&s_audio_mux);
    bool ok = allowed && mic_capture_read(context, true, buffer, bytes, timeout_ms);
    portENTER_CRITICAL(&s_audio_mux);
    s_mic_transition = false;
    portEXIT_CRITICAL(&s_audio_mux);
    xSemaphoreGive(s_mic_read_lock);
    return ok;
}
static void deliver_queued(void *context, const int16_t *frame, const uint8_t *ima, uint32_t epoch) {
    (void)context;
    if (epoch == audio_capture_epoch() && s_mic_fn) s_mic_fn(frame, frame || ima ? MIC_FRAME_SAMPLES : 0, ima);
}
static void task_deliver(void *context, int16_t *frame, const uint8_t *reference, int32_t *dc, uint32_t epoch) {
    (void)context;
    xSemaphoreTake(s_mic_read_lock, portMAX_DELAY);
    if (capture_requested() && epoch == audio_capture_epoch()) {
        if (frame) prepare_voice(frame, dc);
        if (s_wake_capture) { if (s_mic_fn) s_mic_fn(frame, MIC_FRAME_SAMPLES, NULL); }
        else mic_delivery_push(frame, epoch, reference);
    }
    xSemaphoreGive(s_mic_read_lock);
}
static bool task_duplex(void *context) { (void)context; return s_duplex && !s_wake_capture; }
static uint32_t task_epoch(void *context) { (void)context; return audio_capture_epoch(); }
static const mic_task_config_t s_mic_task_config = {
    .context = &s_capture, .requested = task_requested, .sync = task_sync, .read = task_read,
    .deliver = task_deliver, .epoch = task_epoch, .duplex = task_duplex, .slots = AUDIO_CAPTURE_CHANNELS, .chunk_samples = MIC_CHUNK,
    .frame_samples = MIC_FRAME_SAMPLES, .voice_slot = AUDIO_MIC_SLOT, .reference_slot = AUDIO_REFERENCE_SLOT, .timeout_ms = 20,
};
void audio_set_volume(int pct) { audio_levels_set_speech(pct); }
void audio_set_ui_volume(int pct) {
    audio_levels_set_interface(pct); if (pct <= 0) audio_sfx_quiet();
}
void audio_init(int volume) {
    audio_sfx_init();
    audio_ring_init();
    s_mic_read_lock = xSemaphoreCreateMutex();
    assert(s_mic_read_lock);
    ESP_ERROR_CHECK(audio_board_init(&s_capture, &s_out, &s_out_codec, &s_tx) ? ESP_OK : ESP_FAIL);
    audio_set_volume(volume);
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_out, 100));
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(speaker_task, "spk", 4096, NULL, 10, &s_spk_task, KUBIK_AUDIO_CORE) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(mic_capture_task, "mic", 4096, (void *)&s_mic_task_config, 6, &s_mic_task, KUBIK_AUDIO_CORE) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "duplex audio ready, %d Hz, volume %d", AUDIO_RATE, volume);
}
