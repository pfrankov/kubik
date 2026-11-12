#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "printer_layout.h"
#include "printer_stream.h"

typedef struct {
    char rid[33];
    uint32_t session;
    bool automatic, printing, dispatched;
    bool image;
    uint16_t image_width, image_height;
    printer_stream_t *stream;
    atomic_bool cancelled;
    int64_t deadline_ms;
    printer_document_t document;
    char text[];
} printer_job_t;

// Called by the one radio worker. A reply is allocated only after BLE closes.
// Automatic discovery has no wire reply and never sends print bytes.
char *printer_job_run(printer_job_t *job, bool *found);
