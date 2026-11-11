#pragma once
#include "cJSON.h"
#include "printer_emoji.h"

bool printer_emoji_parse_valid(const cJSON *array, const char *text, unsigned *count);
// Caller owns count actual glyph slots; cJSON is borrowed only during this call.
bool printer_emoji_parse_copy(const cJSON *array, printer_emoji_glyph_t *out, unsigned count);
