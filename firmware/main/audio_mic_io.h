#pragma once

#include <stdbool.h>
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "audio_codec_data_if.h"
#include "mic_capture.h"

bool audio_mic_io_init(mic_capture_t *capture, const audio_codec_data_if_t *data,
                       const audio_codec_if_t *codec, i2s_chan_handle_t rx,
                       esp_codec_dev_sample_info_t *format);
