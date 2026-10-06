#include "link_mic.h"
#include "link.h"
#include "ima_adpcm.h"
#include <string.h>

static uint8_t frame[2 + 2 * (IMA_HEADER_BYTES + 960 / 2)];

const uint8_t *link_mic_duplex_preroll(const int16_t *pcm) {
    ima_state_t encoder = {0};
    ima_encode(&encoder, pcm, 960, frame + 2);
    memset(frame + 2 + IMA_HEADER_BYTES + 960 / 2, 0, IMA_HEADER_BYTES + 960 / 2);
    return frame + 2;
}

const uint8_t *link_mic_duplex_encode(uint8_t turn, const uint8_t *ima, size_t *encoded) {
    if (!ima) return NULL;
    frame[0] = 0x05; frame[1] = turn;
    if (ima != frame + 2) memcpy(frame + 2, ima, sizeof frame - 2);
    *encoded = sizeof frame;
    return frame;
}

const uint8_t *link_mic_encode(uint8_t turn, uint32_t session, const int16_t *pcm, size_t bytes, const uint8_t *ima, size_t *encoded) {
    static ima_state_t state;
    static uint32_t last_session;
    static uint8_t last_turn;
    if (!pcm || !bytes || bytes > LINK_MIC_PCM_MAX || bytes % 4) return NULL;
    if (session != last_session || turn != last_turn) state = (ima_state_t){0};
    last_session = session; last_turn = turn;
    frame[0] = 0x04; frame[1] = turn;
    if (ima) memcpy(frame + 2, ima, IMA_HEADER_BYTES + bytes / 4);
    else ima_encode(&state, pcm, bytes / 2, frame + 2);
    *encoded = 2 + IMA_HEADER_BYTES + bytes / 4;
    return frame;
}
