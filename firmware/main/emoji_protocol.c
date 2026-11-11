#include "emoji_protocol.h"
#include "emoji_cluster.h"
#include <string.h>

static int digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    return c == '/' ? 63 : -1;
}

static bool decode(const char *encoded, uint8_t out[SCREEN_EMOJI_BYTES]) {
    if (!encoded || strlen(encoded) != SCREEN_EMOJI_BYTES / 3 * 4) return false;
    unsigned at = 0;
    for (unsigned i = 0; i < SCREEN_EMOJI_BYTES / 3 * 4; i += 4) {
        int a = digit(encoded[i]), b = digit(encoded[i + 1]);
        int c = digit(encoded[i + 2]), d = digit(encoded[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        out[at++] = (uint8_t)((a << 2) | (b >> 4));
        out[at++] = (uint8_t)((b << 4) | (c >> 2));
        out[at++] = (uint8_t)((c << 6) | d);
    }
    return true;
}

static bool empty_color(const uint8_t *color) {
    return !(color[0] | color[1] | color[2] | color[3]);
}

static bool palette_valid(const uint8_t *pixels) {
    if (!empty_color(pixels)) return false;
    uint16_t used = 0;
    for (unsigned i = 64; i < SCREEN_EMOJI_BYTES; i++)
        used |= (uint16_t)((1u << (pixels[i] >> 4)) | (1u << (pixels[i] & 15)));
    if (!(used & 0xfffeu)) return false;
    for (unsigned i = 1; i < 16; i++) {
        const uint8_t *color = pixels + i * 4;
        if (used & (1u << i)) { if (!color[3]) return false; }
        else if (!empty_color(color)) return false;
    }
    return true;
}

static bool id_valid(const cJSON *id) {
    return cJSON_IsNumber(id) && id->valuedouble >= 1 && id->valuedouble <= UINT32_MAX &&
        id->valuedouble == (double)(uint32_t)id->valuedouble;
}

static bool fields_valid(const cJSON *object) {
    if (!cJSON_IsObject(object)) return false;
    unsigned seen = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, object) {
        unsigned bit = !strcmp(item->string, "t") ? 1 : !strcmp(item->string, "id") ? 2 :
            !strcmp(item->string, "seq") ? 4 : !strcmp(item->string, "data") ? 8 : 0;
        if (!bit || (seen & bit)) return false;
        seen |= bit;
    }
    return seen == 15;
}

bool emoji_protocol_parse(const cJSON *object, emoji_reply_t *reply) {
    if (!reply || !fields_valid(object)) return false;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "t"));
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "id");
    const char *sequence = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "seq"));
    if (!type || strcmp(type, "emoji") || !id_valid(id)) return false;
    if (!sequence || !emoji_sequence_valid(sequence, strlen(sequence))) return false;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(object, "data");
    if (!cJSON_IsNull(data) && (!decode(cJSON_GetStringValue(data), reply->pixels) || !palette_valid(reply->pixels))) return false;
    reply->id = (uint32_t)id->valuedouble;
    reply->sequence = sequence;
    reply->available = !cJSON_IsNull(data);
    return true;
}
