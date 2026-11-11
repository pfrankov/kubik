#pragma once
#include "printer_job.h"
#include "peripage_ble.h"

char *printer_result_make(const printer_job_t *job, const char *error,
    const peripage_info_t *printer, const printer_layout_info_t *layout);
