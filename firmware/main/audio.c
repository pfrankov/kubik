#include "audio.h"
#include "audio_levels.h"
#include "audio_signal.h"
#include "audio_sfx.h"
#include <string.h>
#include "assets.h"
#include "tess_sound.h"
#include "board.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ima_adpcm.h"
#include "mic_capture.h"
#include "mic_capture_task.h"
#include "mic_delivery.h"
#include "audio_board.h"
#include "audio_output_tail.h"
static const char *TAG = "audio";
#define SPK_FRAME AUDIO_SPEAKER_FRAMES  // 20 ms
// 900 ms of ADPCM: 100 ms above host credit, leaving RAM for TLS and Wi-Fi.
#define RING_BYTES_PER_MS (AUDIO_RATE / 2 / 1000)
#define RING_BYTES 10800
#define RING_BLOCKS 3
#define RING_BLOCK_BYTES (RING_BYTES / RING_BLOCKS)
_Static_assert(RING_BYTES == 900 * RING_BYTES_PER_MS, "the ring holds 900 ms of speech, above the 800 ms send window");
#define PREBUF_BYTES (600 * RING_BYTES_PER_MS)  // one bounded TCP retry before speech starts
#define REBUF_BYTES (300 * RING_BYTES_PER_MS)   // after running dry mid-speech
#define FRAME_BYTES (SPK_FRAME / 2)
// TDM slots: MIC1, speaker loop-back, MIC2, unused. PTT sends MIC1 alone.
#define MIC_SLOTS 4
#define MIC1_SLOT 0
#define MIC_CHUNK 240  // frames per read (10 ms); MIC_FRAME_SAMPLES is a whole number of them
static bool s_duplex;
static esp_codec_dev_handle_t s_out;
static uint8_t *s_ring[RING_BLOCKS];
static audio_output_tail_t s_output_tail;
static bool s_output_in_flight;  // last decoded frame may not have reached I2S yet
static volatile size_t s_ring_cap;
static SemaphoreHandle_t s_ring_lock;  // begin/write on link task; release on speaker task
static volatile size_t s_rd, s_wr;  // monotonically increasing byte counters
static volatile bool s_prebuf, s_ended, s_active;
static volatile uint32_t s_played_bytes, s_gaps, s_gap_frames, s_mix_max_us, s_write_max_us;
static volatile uint32_t s_stream_epoch;
static volatile size_t s_prebuf_bytes;
static int64_t s_start_deadline_us;
static uint32_t s_pending_gap_frames;
static size_t s_gap_write_at;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
// A stream is one ADPCM sequence; initialize its decoder from the first frame.
static ima_state_t s_dec;
static bool s_dec_set;
static void ring_free_locked(void) {
    for (int i = 0; i < RING_BLOCKS; i++) { heap_caps_free(s_ring[i]); s_ring[i] = NULL; }
    s_ring_cap = 0;
}
static bool ring_acquire_locked(void) {
    if (s_ring_cap) return true;
    // CPU-only ADPCM belongs in LP SRAM; leave ordinary DMA-capable SRAM for Wi-Fi.
    // Two 3600-byte LP blocks fit beside Live's 3072-byte sender stack.
    for (int i = 0; i < RING_BLOCKS; i++) {
        unsigned caps = i < 2 ? MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        s_ring[i] = heap_caps_malloc(RING_BLOCK_BYTES, caps);
        if (!s_ring[i]) { ring_free_locked(); return false; }
    }
    s_ring_cap = RING_BYTES;
    return true;
}
static void ring_release_if_idle(void) {
    if (!s_ring_cap || (s_active && !(s_ended && s_wr == s_rd))) return;
    if (xSemaphoreTake(s_ring_lock, 0) != pdTRUE) return;
    if (!s_active || (s_ended && s_wr == s_rd)) ring_free_locked();
    xSemaphoreGive(s_ring_lock);
}
void audio_stream_begin(void) {
    audio_sfx_quiet();
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    bool have = ring_acquire_locked();
    if (!have) ESP_LOGW(TAG, "speech buffer: %d bytes unavailable, largest free %u", RING_BYTES,
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    portENTER_CRITICAL(&s_ring_mux);
    ++s_stream_epoch;
    s_rd = s_wr = 0;
    s_prebuf = true;
    s_prebuf_bytes = PREBUF_BYTES;
    s_start_deadline_us = 0; s_pending_gap_frames = 0; s_gap_write_at = 0;
    s_gaps = s_gap_frames = s_mix_max_us = 0;
    s_write_max_us = 0;
    s_ended = false;
    s_active = have;
    s_played_bytes = 0;
    s_dec = (ima_state_t){0};
    s_dec_set = false;
    portEXIT_CRITICAL(&s_ring_mux);
    xSemaphoreGive(s_ring_lock);
    audio_kick(); // publish first: the higher-priority speaker must observe active on wake
}
static size_t ring_space(void) { return s_ring_cap - (s_wr - s_rd); }
static void ring_copy(size_t position, uint8_t *linear, size_t bytes, bool into_ring) {
    for (size_t i = 0; i < bytes;) {
        size_t off = (position + i) % s_ring_cap;
        size_t block_offset = off % RING_BLOCK_BYTES;
        size_t n = RING_BLOCK_BYTES - block_offset;
        if (n > bytes - i) n = bytes - i;
        uint8_t *block = s_ring[off / RING_BLOCK_BYTES] + block_offset;
        if (into_ring) memcpy(block, linear + i, n);
        else memcpy(linear + i, block, n);
        i += n;
    }
}
static void ring_put(const uint8_t *src, size_t bytes) {
    ring_copy(s_wr, (uint8_t *)src, bytes, true);
    if (!s_wr && bytes) s_start_deadline_us = esp_timer_get_time() + PREBUF_BYTES / RING_BYTES_PER_MS * 1000;
    portENTER_CRITICAL(&s_ring_mux);
    s_wr += bytes;
    portEXIT_CRITICAL(&s_ring_mux);
}
static size_t stream_write_locked(const uint8_t *frame, size_t bytes) {
    if (!s_active || s_ended || bytes < IMA_HEADER_BYTES) return 0;
    if (!s_dec_set) {
        ima_state_t st;
        if (!ima_read_header(frame, &st)) return 0;
        s_dec = st;  // published to the reader with the data by ring_put
        s_dec_set = true;
    }
    frame += IMA_HEADER_BYTES;
    bytes -= IMA_HEADER_BYTES;
    if (bytes > ring_space()) {
        // Preserve the ADPCM sequence if a pacing bug overflows the ring.
        ESP_LOGW(TAG, "speech buffer overflow: %u ms dropped", (unsigned)(bytes / RING_BYTES_PER_MS));
        return 0;
    }
    ring_put(frame, bytes);
    return bytes;
}
size_t audio_stream_write_ima(const uint8_t *frame, size_t bytes) {
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    size_t written = stream_write_locked(frame, bytes);
    xSemaphoreGive(s_ring_lock);
    return written;
}
void audio_stream_end(void) {
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    portENTER_CRITICAL(&s_ring_mux); s_ended = true; portEXIT_CRITICAL(&s_ring_mux);
    xSemaphoreGive(s_ring_lock);
}
void audio_stream_stop(void) {
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    portENTER_CRITICAL(&s_ring_mux);
    s_rd = s_wr;
    s_ended = true;
    s_active = false;
    portEXIT_CRITICAL(&s_ring_mux);
    xSemaphoreGive(s_ring_lock);
}
void audio_stream_timing(uint32_t *mix_us, uint32_t *write_us) { *mix_us = s_mix_max_us; *write_us = s_write_max_us; }
void audio_stream_gaps(uint32_t *count, uint32_t *ms) { *count = s_gaps; *ms = s_gap_frames * SPK_FRAME * 1000 / AUDIO_RATE; }
static bool output_idle_locked(int64_t now) {
    return !s_output_in_flight && audio_output_tail_ready(&s_output_tail, now);
}
static bool output_idle(void) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_ring_mux);
    bool ready = output_idle_locked(now);
    portEXIT_CRITICAL(&s_ring_mux);
    return ready;
}
bool audio_stream_drained(void) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_ring_mux);
    bool drained = s_ended && s_wr == s_rd && output_idle_locked(now);
    portEXIT_CRITICAL(&s_ring_mux);
    return drained;
}
static void output_written(void) {
    int64_t written = esp_timer_get_time();
    portENTER_CRITICAL(&s_ring_mux);
    audio_output_tail_mark(&s_output_tail, written);
    s_output_in_flight = false;
    portEXIT_CRITICAL(&s_ring_mux);
}
uint32_t audio_stream_played_ms(void) { return s_played_bytes / RING_BYTES_PER_MS; }
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
    bool quiet = s_doze && !s_active && !s_gate && !s_mic_transition && !s_capture.rx_enabled && !s_capture.adc_enabled;
    portEXIT_CRITICAL(&s_audio_mux);
    return quiet && !audio_sfx_playing() && !audio_self_audible() &&
        output_idle();
}
static void doze_while_quiet(void) {
    if (!may_doze()) return;
    portENTER_CRITICAL(&s_audio_mux);
    bool reserved = s_doze && !s_active && !s_gate && !s_mic_transition && !s_capture.rx_enabled && !s_capture.adc_enabled;
    if (reserved) s_codecs_down = true;
    portEXIT_CRITICAL(&s_audio_mux);
    if (!reserved) return;
    int64_t t0 = esp_timer_get_time();
    s_out_codec->enable(s_out_codec, false);
    i2s_channel_disable(s_tx);
    s_speech_audible = false; s_spk_level = 0.f; // no stale speech/work flag while output sleeps
    ESP_LOGI(TAG, "codecs dozing");
    while (may_doze()) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
    i2s_channel_enable(s_tx);
    s_out_codec->enable(s_out_codec, true);
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_out, 100));
    portENTER_CRITICAL(&s_audio_mux);
    s_codecs_down = false;
    portEXIT_CRITICAL(&s_audio_mux);
    xTaskNotifyGive(s_mic_task);
    ESP_LOGI(TAG, "codecs awake after %lld s", (esp_timer_get_time() - t0) / 1000000);
}
static bool decode_speech_frame(size_t available, int16_t *samples, int32_t *mix) {
    if (!available) return false;
    static uint8_t encoded[FRAME_BYTES];
    size_t wanted = available < FRAME_BYTES ? available : FRAME_BYTES;
    ring_copy(s_rd, encoded, wanted, false);
    portENTER_CRITICAL(&s_ring_mux);
    if (s_active) { s_output_in_flight = true; s_rd += wanted; }
    portEXIT_CRITICAL(&s_ring_mux);
    s_played_bytes += wanted;
    ima_decode(&s_dec, encoded, wanted, samples);
    int sample_count = wanted * 2;
    for (int i = 0; i < sample_count; i++) mix[i] += samples[i];
    return sample_count > 0;
}
static bool prebuffer_ready(size_t available) {
    if (available >= s_prebuf_bytes || s_ended) return true;
    // A short Live answer may never fill the buffer; do not wait for a provider
    // end marker (the continuous SDK supplies only a later quiet boundary).
    return !s_played_bytes && available && esp_timer_get_time() >= s_start_deadline_us;
}
static void prebuffer_resume(size_t available) {
    s_prebuf = false;
    // Commit only a pause followed by more speech. A quiet terminal tail is not
    // an audible interruption and must not inflate starvation diagnostics.
    if (available && s_wr != s_gap_write_at && s_pending_gap_frames) { s_gaps++; s_gap_frames += s_pending_gap_frames; }
    s_pending_gap_frames = 0;
}
static bool read_speech_frame_locked(int16_t *samples, int32_t *mix) {
    if (!s_active) return false;
    size_t available = s_wr - s_rd;
    if (s_prebuf && prebuffer_ready(available)) prebuffer_resume(available);
    if (!s_prebuf && !s_ended && available < FRAME_BYTES) {
        s_prebuf = true;
        s_prebuf_bytes = REBUF_BYTES;
        s_gap_write_at = s_wr;
    }
    if (s_prebuf && s_played_bytes) s_pending_gap_frames++;
    return !s_prebuf && decode_speech_frame(available, samples, mix);
}
// Restart/stop cannot reset the decoder or cursors during an old frame's read.
// The codec write remains outside this mutex: at most one already mixed 20ms frame can be in flight.
static bool read_speech_frame(int16_t *samples, int32_t *mix) {
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    bool have = read_speech_frame_locked(samples, mix);
    xSemaphoreGive(s_ring_lock);
    return have;
}
static void mix_speaker_frame(int16_t *samples, int32_t *mix, bool have_speech,
                              float level_history[4], int *history_index) {
    for (int i = 0; i < SPK_FRAME; i++) samples[i] = audio_levels_speech(mix[i]);
    float level = have_speech ? audio_signal_level(samples, SPK_FRAME, -48.f, 40.f) : 0.f;
    memset(mix, 0, SPK_FRAME * sizeof(*mix));
    audio_sfx_mix(mix, SPK_FRAME);
    bool ui_audible = false;
    for (int i = 0; i < SPK_FRAME; i++) {
        int32_t ui = audio_levels_interface(mix[i]);
        ui_audible |= ui != 0;
        int32_t value = audio_levels_mix(samples[i] + ui);
        samples[i] = value > 32767 ? 32767 : value < -32768 ? -32768 : (int16_t)value;
    }
    audio_sfx_note_output(ui_audible);
    s_spk_level = level_history[*history_index];
    level_history[*history_index] = level;
    *history_index = (*history_index + 1) & 3;
    s_speech_audible = have_speech && audio_levels_speech_enabled();
}
static void speaker_cycle(float level_history[4], int *history_index) {
    static int16_t samples[SPK_FRAME];
    static int32_t mix[SPK_FRAME];
    uint32_t epoch = s_stream_epoch;
    int64_t started = esp_timer_get_time();
    memset(mix, 0, sizeof mix);
    bool have_speech = read_speech_frame(samples, mix);
    ring_release_if_idle();
    mix_speaker_frame(samples, mix, have_speech, level_history, history_index);
    int64_t mixed = esp_timer_get_time();
    if (epoch == s_stream_epoch && s_active && mixed - started > s_mix_max_us) s_mix_max_us = mixed - started;
    esp_codec_dev_write(s_out, samples, sizeof samples);
    if (have_speech) output_written();
    uint32_t waited = (uint32_t)(esp_timer_get_time() - mixed);
    if (epoch == s_stream_epoch && s_active && s_played_bytes && waited > s_write_max_us) s_write_max_us = waited;
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
    if (epoch == audio_capture_epoch()) {
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
    .deliver = task_deliver, .epoch = task_epoch, .duplex = task_duplex, .slots = MIC_SLOTS, .chunk_samples = MIC_CHUNK,
    .frame_samples = MIC_FRAME_SAMPLES, .voice_slot = MIC1_SLOT, .timeout_ms = 20,
};
void audio_set_volume(int pct) { audio_levels_set_speech(pct); }
void audio_set_ui_volume(int pct) {
    audio_levels_set_interface(pct); if (pct <= 0) audio_sfx_quiet();
}
void audio_init(int volume) {
    audio_sfx_init();
    s_ring_lock = xSemaphoreCreateMutex();
    s_mic_read_lock = xSemaphoreCreateMutex();
    assert(s_ring_lock);
    assert(s_mic_read_lock);
    if (!audio_board_init(&s_capture, &s_out, &s_out_codec, &s_tx)) return;
    audio_set_volume(volume);
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_out, 100));
    ESP_ERROR_CHECK(xTaskCreate(speaker_task, "spk", 4096, NULL, 7, &s_spk_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(mic_capture_task, "mic", 4096, (void *)&s_mic_task_config, 6, &s_mic_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "duplex audio ready, %d Hz, volume %d", AUDIO_RATE, volume);
}
