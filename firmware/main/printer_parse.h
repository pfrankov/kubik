#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "printer_layout.h"

typedef struct {
    const char *rid;
    bool printing;
    printer_document_t document;
} printer_input_t;

const char *printer_parse_id(const cJSON *request);
bool printer_parse_request(cJSON *request, const char *raw, size_t len,
    size_t consumed, printer_input_t *input);
