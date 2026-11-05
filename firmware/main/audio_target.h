#pragma once
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
// C6 ES7210: MIC1, MIC3 (speaker reference), MIC2, MIC4.
// S3 1.8 ES8311: ADC microphone left, DAC reference right.
#if CONFIG_IDF_TARGET_ESP32S3
#define AUDIO_CAPTURE_CHANNELS 2
#else
#define AUDIO_CAPTURE_CHANNELS 4
#endif
#define AUDIO_MIC_SLOT 0
#define AUDIO_REFERENCE_SLOT 1
