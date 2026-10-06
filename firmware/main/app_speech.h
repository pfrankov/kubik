// Playback belongs to the authenticated connection that started it, even when generations repeat.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>

extern atomic_int s_gen; // active generation, or -1; observation by the UI/power coordinator
void app_speech_init(void);
void app_speech_begin(int gen, uint32_t session);
bool app_speech_matches(int gen, uint32_t session);
void app_speech_write(int gen, uint32_t session, const uint8_t *frame, size_t bytes);
bool app_speech_end(int gen, uint32_t session);
bool app_speech_cancel(int gen, uint32_t session);
void app_speech_stop(bool tell_server);
bool app_speech_tick(int64_t now_ms, bool progress);
