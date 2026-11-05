// Physical talk control for each supported board; USB PTT events use the same voice state machine.
#pragma once
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3
#define KUBIK_VOICE_BUTTON "BOOT"
#define KUBIK_VOICE_BUTTON_IS_BOOT 1
#define KUBIK_VOICE_TAP_ONLY 1
#define KUBIK_TALK_INSTRUCTION "Press " KUBIK_VOICE_BUTTON " to talk"
#define KUBIK_MANUAL_TALK "Press " KUBIK_VOICE_BUTTON ", then talk."
#define KUBIK_MANUAL_SEND "Press again to send."
#define KUBIK_MANUAL_TALK_HINT "Or press " KUBIK_VOICE_BUTTON " twice."
#else
#define KUBIK_VOICE_BUTTON "KEY"
#define KUBIK_VOICE_BUTTON_IS_BOOT 0
#define KUBIK_VOICE_TAP_ONLY 0
#define KUBIK_TALK_INSTRUCTION "Hold the button to talk"
#define KUBIK_MANUAL_TALK "Hold " KUBIK_VOICE_BUTTON " to talk."
#define KUBIK_MANUAL_SEND "Release to send."
#define KUBIK_MANUAL_TALK_HINT "Or hold " KUBIK_VOICE_BUTTON "; release to send."
#endif
