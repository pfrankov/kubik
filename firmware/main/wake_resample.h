#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { int16_t history[16]; unsigned position, phase; } wake_resample_t;
// 24 kHz -> 16 kHz. Input sizes must be multiples of 3; output capacity >= 2*n/3.
size_t wake_resample(wake_resample_t *state, const int16_t *pcm, size_t n, int16_t *out);
