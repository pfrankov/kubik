#include "face.h"
#include "face_emoji.h"
#include "emoji_protocol.h"
#include "status_json.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static face_t g_face;
static bool s_online, s_setup, s_menu;
static int s_power, locks, requests;
static uint32_t session = 7;
static char last_request[512];
static uint32_t link_session(void) { return session; }
static void face_lock(void) { assert(!locks); locks++; }
static void face_unlock(void) { assert(locks == 1); locks--; }
static bool power_is_dark(int power) { return power == 2; }
static bool agent_protocol_has_nul_escape(const char *raw, size_t bytes) {
    (void)bytes; return strstr(raw, "\\u0000") != NULL;
}
static bool link_send_json_in_session(const char *wire, uint32_t expected) {
    assert(!locks && expected == session);
    assert(strlen(wire) < sizeof last_request);
    strcpy(last_request, wire); requests++;
    return true;
}
/* PRODUCTION_APP_EMOJI */

static cJSON *reply_object(uint32_t id, const char *seq, const char *data) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "t", "emoji"); cJSON_AddNumberToObject(j, "id", id);
    cJSON_AddStringToObject(j, "seq", seq);
    if (data) cJSON_AddStringToObject(j, "data", data); else cJSON_AddNullToObject(j, "data");
    return j;
}
static void encode(const uint8_t pixels[576], char encoded[769]) {
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned at = 0;
    for (unsigned i = 0; i < 576; i += 3) {
        uint32_t n = (uint32_t)pixels[i] << 16 | (uint32_t)pixels[i + 1] << 8 | pixels[i + 2];
        encoded[at++] = alphabet[n >> 18]; encoded[at++] = alphabet[(n >> 12) & 63];
        encoded[at++] = alphabet[(n >> 6) & 63]; encoded[at++] = alphabet[n & 63];
    }
    encoded[768] = 0;
}
static void protocol_boundary(const char *encoded) {
    emoji_reply_t result;
    cJSON *j = reply_object(UINT32_MAX, "👩‍💻", encoded);
    assert(emoji_protocol_parse(j, &result) && result.id == UINT32_MAX && result.available);
    cJSON_SetNumberValue(cJSON_GetObjectItem(j, "id"), 1.5); assert(!emoji_protocol_parse(j, &result));
    cJSON_SetNumberValue(cJSON_GetObjectItem(j, "id"), 0); assert(!emoji_protocol_parse(j, &result));
    cJSON_SetNumberValue(cJSON_GetObjectItem(j, "id"), 4294967296.); assert(!emoji_protocol_parse(j, &result));
    cJSON_SetNumberValue(cJSON_GetObjectItem(j, "id"), 1);
    cJSON_AddNumberToObject(j, "extra", 1); assert(!emoji_protocol_parse(j, &result));
    cJSON_DeleteItemFromObject(j, "extra");
    cJSON_AddStringToObject(j, "seq", "😀"); assert(!emoji_protocol_parse(j, &result)); cJSON_Delete(j);
    j = reply_object(1, "text", encoded); assert(!emoji_protocol_parse(j, &result)); cJSON_Delete(j);
    j = reply_object(1, "😀", NULL); assert(emoji_protocol_parse(j, &result) && !result.available); cJSON_Delete(j);
    char bad[769]; strcpy(bad, encoded); bad[23] = '\n';
    j = reply_object(1, "😀", bad); assert(!emoji_protocol_parse(j, &result)); cJSON_Delete(j);
    memset(bad, 'A', 768); bad[768] = 0;
    j = reply_object(1, "😀", bad); assert(!emoji_protocol_parse(j, &result)); cJSON_Delete(j);
}
static void receive(uint32_t id, const char *seq, const char *encoded) {
    cJSON *j = reply_object(id, seq, encoded);
    char raw[1024]; assert(cJSON_PrintPreallocated(j, raw, sizeof raw, false));
    assert(app_emoji_receive(j, raw, strlen(raw), strlen(raw)));
    cJSON_Delete(j); // cache must own every byte after RX JSON destruction
}
static void route_and_page(const char *encoded) {
    s_online = true; g_face.mode = MODE_IDLE;
    face_card(&g_face, "Привет 😀 ❤️ 👍🏽 👩‍💻 🇷🇺");
    app_emoji_poll(); assert(requests == 1);
    cJSON *request = cJSON_Parse(last_request);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(request, "seq")) == 4); cJSON_Delete(request);
    uint32_t id = g_face.card_emoji.id;
    app_emoji_poll(); assert(requests == 1);
    receive(id - 1, "😀", encoded); assert(!g_face.card_emoji.glyphs.ready[0]);
    assert(!render_emoji_entries_live());
    receive(id, "🎉", encoded); assert(!g_face.card_emoji.glyphs.ready[0]);
    receive(id, "😀", encoded); assert(g_face.card_emoji.glyphs.ready[0] == 1);
    assert(render_emoji_entries_live()==1);receive(id,"😀",encoded);assert(render_emoji_entries_live()==1);
    assert(render_emoji_entry_pixels(g_face.card_emoji.glyphs.entries[0])[7] == 255);
    receive(id, "❤️", NULL); assert(g_face.card_emoji.glyphs.ready[1] == 2);
    assert(face_card_tap(&g_face)); app_emoji_poll(); assert(requests == 2);
    receive(id, "🇷🇺", encoded); assert(!g_face.card_emoji.glyphs.ready[0]);
    receive(g_face.card_emoji.id, "🇷🇺", encoded); assert(g_face.card_emoji.glyphs.ready[0] == 1);
    session++; app_emoji_poll(); assert(requests == 3 && g_face.card_emoji.id != id);
    receive(id, "🇷🇺", encoded); assert(!g_face.card_emoji.glyphs.ready[0]);
    s_online = false; app_emoji_poll(); receive(g_face.card_emoji.id, "🇷🇺", encoded);
    assert(!g_face.card_emoji.glyphs.ready[0]);
    s_online = true; s_menu = true; app_emoji_poll(); assert(requests == 3);
    s_menu = false; s_power = 2; app_emoji_poll(); assert(requests == 3);
    s_power = 0; app_emoji_poll(); assert(requests == 4);
    face_emoji_update(&g_face, 3.1f); receive(g_face.card_emoji.id, "🇷🇺", encoded);
    assert(g_face.card_emoji.glyphs.ready[0] == 2);
    char info[512]; status_json_t json; status_json_begin(&json, info, sizeof info); app_emoji_info(&json);
    assert(status_json_end(&json) && strstr(info, "\"emoji_missing\":1"));
}
int main(void) {
    uint8_t pixels[576] = {0}; pixels[4] = 255; pixels[7] = 255; memset(pixels + 64, 0x11, 512);
    char encoded[769]; encode(pixels, encoded);
    protocol_boundary(encoded); route_and_page(encoded);
    face_emoji_reset(&g_face);assert(!render_emoji_entries_live());
    puts("screen emoji: strict glyph parsing, real RX/cache ownership, route epochs, bounded requests and page status passed");
}
