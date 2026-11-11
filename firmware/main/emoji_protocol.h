#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

// A screen glyph: 16 straight RGBA8888 colors and 32x32 packed palette indices.
#define SCREEN_EMOJI_BYTES 576
typedef struct {
    uint32_t id;
    const char *sequence;
    bool available;
    uint8_t pixels[SCREEN_EMOJI_BYTES];
} emoji_reply_t;
bool emoji_protocol_parse(const cJSON *object, emoji_reply_t *reply);
