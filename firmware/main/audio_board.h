#pragma once
#include <stdbool.h>
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "audio_codec_if.h"
#include "mic_capture.h"

bool audio_board_init(mic_capture_t *capture, esp_codec_dev_handle_t *output,
                      const audio_codec_if_t **out_codec, i2s_chan_handle_t *tx);
