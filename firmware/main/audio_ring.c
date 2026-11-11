#include "audio.h"
#include "audio_ring.h"
#include "audio_sfx.h"
#include "audio_target.h"
#include "audio_output_tail.h"
#include "ima_adpcm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <assert.h>
#include <string.h>
static const char *TAG="audio";
#define SPK_FRAME AUDIO_SPEAKER_FRAMES
#define RING_BYTES_PER_MS (AUDIO_RATE / 2 / 1000)
#define RING_BYTES 10800
#define RING_BLOCKS 9
#define RING_BLOCK_BYTES (RING_BYTES / RING_BLOCKS)
_Static_assert(RING_BYTES == 900 * RING_BYTES_PER_MS, "the ring holds 900 ms of speech, above the 800 ms send window");
#define PREBUF_BYTES (800 * RING_BYTES_PER_MS)  // fill the existing host credit before playback
#define REBUF_BYTES (300 * RING_BYTES_PER_MS)   // after running dry mid-speech
#define FRAME_BYTES (SPK_FRAME / 2)
static uint8_t *s_ring[RING_BLOCKS];
static atomic_bool s_ble_reserved;
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
static ima_state_t s_dec;
static bool s_dec_set;
static void ring_free_locked(void) {
    for (int i = 0; i < RING_BLOCKS; i++) { heap_caps_free(s_ring[i]); s_ring[i] = NULL; }
    s_ring_cap = 0;
}
static bool ring_acquire_locked(void) {
    if (s_ring_cap) return true;
    // CPU-only ADPCM: C6 LP SRAM / S3 PSRAM; preserve internal DMA memory.
    for (int i = 0; i < RING_BLOCKS; i++) {
#if CONFIG_IDF_TARGET_ESP32S3
        unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
        unsigned caps = i < 6 ? MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
        s_ring[i] = heap_caps_malloc(RING_BLOCK_BYTES, caps);
        if (!s_ring[i]) { ring_free_locked(); return false; }
    }
    s_ring_cap = RING_BYTES;
    return true;
}
void audio_ring_release_idle(void) {
    if (!s_ring_cap || (s_active && !(s_ended && s_wr == s_rd))) return;
    if (xSemaphoreTake(s_ring_lock, 0) != pdTRUE) return;
    if (!s_active || (s_ended && s_wr == s_rd)) ring_free_locked();
    xSemaphoreGive(s_ring_lock);
}
bool audio_stream_begin(void) {
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    if (atomic_load(&s_ble_reserved)) {
        xSemaphoreGive(s_ring_lock);
        ESP_LOGW(TAG, "speech refused: Bluetooth owns output memory");
        return false;
    }
    bool have = ring_acquire_locked();
    if (!have) ESP_LOGW(TAG, "speech buffer: %d bytes unavailable, largest free %u", RING_BYTES,
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!have) { xSemaphoreGive(s_ring_lock); return false; }
    portENTER_CRITICAL(&s_ring_mux);
    ++s_stream_epoch;
    s_rd = s_wr = s_played_bytes = 0;
    s_prebuf = true;
    s_prebuf_bytes = PREBUF_BYTES;
    s_start_deadline_us = 0; s_pending_gap_frames = 0; s_gap_write_at = 0;
    s_gaps = s_gap_frames = s_mix_max_us = s_write_max_us = 0;
    s_ended = false;
    s_active = have;
    s_dec = (ima_state_t){0};
    s_dec_set = false;
    portEXIT_CRITICAL(&s_ring_mux);
    xSemaphoreGive(s_ring_lock);
    audio_sfx_quiet();
    audio_kick(); // publish first: the higher-priority speaker must observe active on wake
    return true;
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
bool audio_ring_output_idle(void) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_ring_mux);
    bool ready = output_idle_locked(now);
    portEXIT_CRITICAL(&s_ring_mux);
    return ready;
}
bool audio_ble_reserve(void) {
    if (!s_ring_lock || xSemaphoreTake(s_ring_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    portENTER_CRITICAL(&s_ring_mux);
    bool idle = !atomic_load(&s_ble_reserved) && s_rd == s_wr && (!s_active || s_ended) &&
        output_idle_locked(esp_timer_get_time());
    if (idle) atomic_store(&s_ble_reserved, true);
    portEXIT_CRITICAL(&s_ring_mux);
    if (idle) ring_free_locked();
    xSemaphoreGive(s_ring_lock);
    return idle;
}
void audio_ble_release(void) { atomic_store(&s_ble_reserved, false); }
static bool stream_busy(bool require_active) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_ring_mux);
    bool busy = ((!require_active || s_active) && (!s_ended || s_wr != s_rd)) || !output_idle_locked(now);
    portEXIT_CRITICAL(&s_ring_mux);
    return busy;
}
bool audio_stream_drained(void) { return !stream_busy(false); }
bool audio_stream_busy(void) { return stream_busy(true); }
void audio_ring_output_written(void) {
    int64_t written = esp_timer_get_time();
    portENTER_CRITICAL(&s_ring_mux);
    audio_output_tail_mark(&s_output_tail, written);
    s_output_in_flight = false;
    portEXIT_CRITICAL(&s_ring_mux);
}
uint32_t audio_stream_played_ms(void) { return s_played_bytes / RING_BYTES_PER_MS; }
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
bool audio_ring_read(int16_t *samples, int32_t *mix) {
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    bool have = read_speech_frame_locked(samples, mix);
    xSemaphoreGive(s_ring_lock);
    return have;
}

void audio_ring_init(void) { s_ring_lock=xSemaphoreCreateMutex(); assert(s_ring_lock); }
bool audio_ring_active(void) { return s_active; }
uint32_t audio_ring_epoch(void) { return s_stream_epoch; }
void audio_ring_note_mix(uint32_t epoch,uint32_t us) {
    if(epoch==s_stream_epoch && s_active && us>s_mix_max_us)s_mix_max_us=us;
}
void audio_ring_note_write(uint32_t epoch,uint32_t us) {
    if(epoch==s_stream_epoch && s_active && s_played_bytes && us>s_write_max_us)s_write_max_us=us;
}
