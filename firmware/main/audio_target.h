#pragma once
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
// C6 ES7210 wire slots: MIC1, MIC3 (speaker reference), MIC2, MIC4.
// RX DMA selects only the first two; keep all four slots in the codec clock.
// S3 1.8 ES8311 REG44 mode4: DACL speaker reference, ADC microphone.
#define AUDIO_CAPTURE_CHANNELS 2
#if CONFIG_IDF_TARGET_ESP32S3
#define AUDIO_CAPTURE_FRAME_SLOTS 2
#define AUDIO_MIC_SLOT 1
#define AUDIO_REFERENCE_SLOT 0
#else
#define AUDIO_CAPTURE_FRAME_SLOTS 4
#define AUDIO_MIC_SLOT 0
#define AUDIO_REFERENCE_SLOT 1
#endif
