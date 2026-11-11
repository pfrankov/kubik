#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "printer_parse.h"
#include "printer_platform.h"

typedef struct {
    printer_app_state_t state;
    uint32_t observed_session;
    unsigned attempts;
    int64_t due_in_ms;
    bool busy, discovered, guard_allowed;
} printer_diagnostics_t;
void printer_work_diagnostics(printer_diagnostics_t *diagnostics);

typedef enum {
    PRINTER_ADMITTED, PRINTER_DUPLICATE, PRINTER_WORK_BUSY, PRINTER_WORK_FAILED
} printer_admission_t;

printer_admission_t printer_work_submit(const printer_input_t *input, uint32_t session);
// App task only. Starts work and returns at most one owned JSON reply.
bool printer_work_poll(char **reply, uint32_t *session);
bool printer_work_busy(void);
bool printer_work_discovered(void);
bool printer_work_has_id(const char *rid);
// Cancel background discovery; false while its resources still prevent voice.
// Accepted foreground jobs leave continuous Live input unchanged.
bool printer_work_before_voice(void);
