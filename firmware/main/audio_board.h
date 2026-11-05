#pragma once
#include <stdbool.h>
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "audio_codec_if.h"
#include "mic_capture.h"

bool audio_board_init(mic_capture_t *capture, esp_codec_dev_handle_t *output,
                      const audio_codec_if_t **out_codec, i2s_chan_handle_t *tx);

// S3 ES8311 shares its clock/DAC with the microphone; gate only its ADC/PGA.
int audio_board_adc_enable(bool enabled);
