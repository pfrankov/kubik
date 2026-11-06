#include "audio_mic_io.h"
#include "audio_board.h"
#include "audio_target.h"

#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_attr.h"
#include <stdatomic.h>

#define MIC_GAIN_MASK ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0)
#define MIC_GAIN_DB 30.0

typedef struct {
    const audio_codec_data_if_t *data;
    const audio_codec_if_t *codec;
    i2s_chan_handle_t rx;
} mic_io_t;
static mic_io_t s_io;
static DRAM_ATTR atomic_uint s_dropped_ms;
static bool IRAM_ATTR rx_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *context) {
    (void)channel; (void)context;
    // Count lost time using this board's native channel count.
    atomic_fetch_add_explicit(&s_dropped_ms, event->size / (AUDIO_CAPTURE_CHANNELS * 24 * 2), memory_order_relaxed);
    return false;
}
uint32_t audio_mic_dropped_ms(void) { return atomic_load(&s_dropped_ms); }

static int set_rx(void *context, bool enabled) {
    mic_io_t *io = context;
    return io->data->enable(io->data, ESP_CODEC_DEV_TYPE_IN, enabled);
}
static int set_adc(void *context, bool enabled) {
    mic_io_t *io = context;
#if CONFIG_IDF_TARGET_ESP32S3
    int result = audio_board_adc_enable(enabled);
    (void)io; // Shared ES8311 power and its distinct gain stages belong to the board.
#else
    int result = io->codec->enable(io->codec, enabled);
    if (result == ESP_CODEC_DEV_OK && enabled)
        result = io->codec->set_mic_channel_gain(io->codec, MIC_GAIN_MASK, MIC_GAIN_DB);
#endif
    return result;
}
static int read_rx(void *context, void *buffer, size_t bytes, size_t *read, uint32_t timeout_ms) {
    mic_io_t *io = context;
    return i2s_channel_read(io->rx, buffer, bytes, read, timeout_ms);
}

bool audio_mic_io_init(mic_capture_t *capture, const audio_codec_data_if_t *data,
                       const audio_codec_if_t *codec, i2s_chan_handle_t rx,
                       esp_codec_dev_sample_info_t *format) {
    s_io = (mic_io_t){.data = data, .codec = codec, .rx = rx};
    const i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = rx_overflow};
    if (i2s_channel_register_event_callback(rx, &callbacks, NULL) != ESP_OK) return false;
    if (data->set_fmt(data, ESP_CODEC_DEV_TYPE_IN, format) != ESP_CODEC_DEV_OK ||
        codec->set_fs(codec, format) != ESP_CODEC_DEV_OK) return false;
    mic_capture_init(capture, &s_io, set_rx, set_adc, read_rx, false, false);
    return true;
}
