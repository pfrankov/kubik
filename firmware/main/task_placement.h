// Keep the UI/state actors serialized; run media and system work separately on S3.
#pragma once
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if CONFIG_IDF_TARGET_ESP32S3
#if CONFIG_FREERTOS_UNICORE
#error "ESP32-S3 requires dual-core: disable CONFIG_FREERTOS_UNICORE"
#endif
#define KUBIK_UI_CORE 1
#else
#define KUBIK_UI_CORE 0
#endif
#define KUBIK_AUDIO_CORE 0
