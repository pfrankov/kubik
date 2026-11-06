#pragma once
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
// C6 ES7210: MIC1, MIC3 (speaker reference), MIC2, MIC4.
// S3 1.8 ES8311 REG44 mode4: DACL speaker reference, ADC microphone.
#if CONFIG_IDF_TARGET_ESP32S3
#define AUDIO_CAPTURE_CHANNELS 2
#define AUDIO_MIC_SLOT 1
#define AUDIO_REFERENCE_SLOT 0
#else
#define AUDIO_CAPTURE_CHANNELS 4
#define AUDIO_MIC_SLOT 0
#define AUDIO_REFERENCE_SLOT 1
#endif
