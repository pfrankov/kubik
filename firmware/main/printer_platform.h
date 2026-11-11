#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t session;
    // Start blockers bits0..7: disabled/offline/setup/pairing/power/capture/speech/audio.
    // Automatic-only blockers bits8..11: Live/talk/server/agent activity.
    uint32_t blockers;
    bool online, enabled, can_start, automatic_allowed;
} printer_app_state_t;

// Snapshot is produced on the app task, where UI/voice ownership lives.
void printer_app_state(printer_app_state_t *state);
void printer_platform_boot(void);
int printer_platform_reset_reason(void);
int64_t printer_platform_now_ms(void);
bool printer_platform_launch(void (*run)(void *), void *context);
bool printer_platform_finished(void);
void printer_platform_reap(void);
void printer_platform_release(void);
void printer_platform_wait_tick(void);
// A controller which cannot release its resources is never reused.
void printer_platform_radio_fault(void);
