#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EMOJI_CLUSTER_MAX_BYTES 64

// Strict UTF-8 scalar decoder; failure leaves cursor unchanged.
bool emoji_utf8_read(const char **cursor, const char *end, uint32_t *codepoint);
// A renderable glyph key must be one complete, structurally valid emoji sequence.
bool emoji_sequence_valid(const char *sequence, size_t bytes);
// One ordinary scalar or one whole emoji compound, including unsupported and
// over-64-byte compounds. is_emoji classifies the start, not atlas availability.
// Empty/null input returns 0. Malformed UTF-8 advances one byte with false.
size_t emoji_cluster_next(const char *text, size_t bytes, bool *is_emoji);
