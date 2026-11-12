#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

bool screen_image_receive(const cJSON *request, const char *raw, size_t len, size_t consumed, uint32_t session);
void screen_image_poll(void);
