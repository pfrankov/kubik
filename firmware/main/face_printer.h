#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include "printer_status.h"
#include "viewport.h"

typedef struct {
    bool open, refresh;
    printer_status_t status;
} face_printer_t;

static inline bool face_printer_back(face_printer_t *printer) {
    if (!printer->open) return false;
    printer->open = printer->refresh = false;
    return true;
}

static inline bool face_printer_tap(face_printer_t *printer, int x, int y) {
    if (!printer->open) return false;
    if (!printer->status.busy && printer->status.can_refresh &&
        abs(x - 240) <= 220 && abs(y - UI_FOOTER_Y) <= 32) printer->refresh = true;
    return true;
}
