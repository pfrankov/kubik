#pragma once
#include <stddef.h>
#include <stdint.h>
const uint8_t *link_mic_duplex_preroll(const int16_t *pcm);
const uint8_t *link_mic_duplex_encode(uint8_t turn, const uint8_t *ima, size_t *encoded);
// Single microphone sender owns this buffer until the synchronous send returns.
const uint8_t *link_mic_encode(uint8_t turn, uint32_t session, const int16_t *pcm, size_t bytes, const uint8_t *ima, size_t *encoded);
