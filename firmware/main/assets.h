// Plush assets (tools/assets/pack.py): animation and sound kit, mapped through the flash MMU.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void assets_sprites(bool enabled);
bool assets_init(void);  // maps the directory and the audio; hooks up the sprite pack
// Audio clip (sfx/<name>): 24 kHz mono s16le, permanently mapped.
const uint8_t *assets_data(const char *name, uint32_t *bytes);
const int16_t *assets_pcm(const char *name, uint32_t *samples);

#ifdef __cplusplus
}
#endif
