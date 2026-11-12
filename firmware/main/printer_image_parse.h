#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "printer_parse.h"

typedef enum { PRINTER_IMAGE_INVALID, PRINTER_IMAGE_START, PRINTER_IMAGE_ABORT } printer_image_action_t;
printer_image_action_t printer_image_parse(cJSON *request, const char *raw, size_t len,
                                           size_t consumed, printer_input_t *input);
