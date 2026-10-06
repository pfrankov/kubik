#pragma once
#include <stdbool.h>
#include <stddef.h>
// Routes mbedTLS allocations through a reserved slot for record buffers (see tls_mem.c).
void tls_mem_init(void);
// Reserve a full TLS record only while speech owns RAM; release waits for an in-flight record.
void tls_mem_speech(bool active);

// Acquire before VAD/microphone allocations; retain through waiting, release when the turn is idle.
bool tls_mem_turn(bool active);

// Capture and playback use the same turn workspace in separate phases.
// A failed take leaves any active TLS record untouched; release only after worker join.
void *tls_mem_capture_take(size_t bytes);
void tls_mem_capture_release(void);
