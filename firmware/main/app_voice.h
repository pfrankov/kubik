#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

#include "voice_mode.h"

voice_mode_t app_voice_mode(void);
bool app_voice_live_active(void);
bool app_voice_live_ready(void);
void app_voice_live_stop(bool tell_server);
int app_voice_parse_generation(cJSON *json);
bool app_voice_receive(cJSON *json, const char *type);
void app_voice_capture_started(uint8_t turn);
void app_voice_prepare_capture(uint8_t turn);
bool app_voice_start_capture(void);
void app_voice_recording_stop(void);
void app_voice_capture_failed(void);
void app_voice_input_end(int packed_turn_reason, int capture_epoch, uint32_t session);
void app_voice_capabilities(int stt, int flags);
