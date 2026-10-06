// Pause endpoint for wake turns and native voice; classic KEY keeps hold/latch behavior.
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint32_t elapsed_ms, voiced_ms, quiet_ms, warmup_ms; bool heard; } auto_voice_t;
typedef enum { AUTO_CONTINUE, AUTO_SEND, AUTO_EMPTY } auto_voice_result_t;
auto_voice_result_t auto_voice_step(auto_voice_t *state, bool speech, bool muted, unsigned ms);
