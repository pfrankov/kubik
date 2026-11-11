#include "app_emoji.h"
#include "app_internal.h"
#include "agent_protocol.h"
#include "emoji_protocol.h"
#include "face_emoji.h"
#include <string.h>
#include <stdatomic.h>

static _Atomic uint32_t s_session;

static bool raw_valid(const char *raw, size_t bytes, size_t parsed) {
    if (parsed > bytes || agent_protocol_has_nul_escape(raw, bytes)) return false;
    for (size_t i = parsed; i < bytes; i++)
        if (raw[i] != ' ' && raw[i] != '\r' && raw[i] != '\n' && raw[i] != '\t') return false;
    return true;
}

bool app_emoji_receive(const cJSON *object, const char *raw, size_t bytes, size_t parsed) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "t"));
    if (!type || strcmp(type, "emoji")) return false;
    uint32_t session = link_session();
    // Only the app task reads s_online. RX uses the atomic epoch admitted by
    // that task after welcome; route loss advances link_session immediately.
    if (!session || session != s_session || !raw_valid(raw, bytes, parsed)) return true;
    emoji_reply_t reply;
    if (!emoji_protocol_parse(object, &reply)) return true;
    face_lock();
    if (session == link_session())
        face_emoji_receive(&g_face, reply.id, reply.sequence, reply.available ? reply.pixels : NULL);
    face_unlock();
    return true;
}

static void send_request(uint32_t session, uint32_t id, char keys[4][65], unsigned count) {
    cJSON *request = cJSON_CreateObject();
    cJSON *sequences = cJSON_AddArrayToObject(request, "seq");
    bool ok = sequences && cJSON_AddStringToObject(request, "t", "emoji_get") &&
        cJSON_AddNumberToObject(request, "id", id);
    for (unsigned i = 0; ok && i < count; i++) ok = cJSON_AddItemToArray(sequences, cJSON_CreateString(keys[i]));
    char wire[512];
    if (ok && cJSON_PrintPreallocated(request, wire, sizeof wire, false)) link_send_json_in_session(wire, session);
    cJSON_Delete(request);
}

void app_emoji_poll(void) {
    uint32_t session = s_online ? link_session() : 0;
    if (session != s_session) {
        face_lock(); face_emoji_reset(&g_face); face_unlock();
        s_session = session;
    }
    if (!session || s_setup || s_menu || power_is_dark(s_power)) return;
    char keys[4][65];
    uint32_t id;
    unsigned count;
    face_lock();
    bool request = face_emoji_poll(&g_face, &id, keys, &count);
    face_unlock();
    if (request) send_request(session, id, keys, count);
}

void app_emoji_info(status_json_t *info) {
    face_lock();
    face_emoji_stats_t stats;
    face_emoji_stats(&g_face, &stats);
    face_unlock();
    status_json_integer(info, "emoji_id", stats.id);
    status_json_integer(info, "emoji_count", stats.count);
    status_json_integer(info, "emoji_ready", stats.ready);
    status_json_integer(info, "emoji_missing", stats.missing);
    status_json_integer(info, "card_page", stats.page);
    status_json_integer(info, "card_pages", stats.pages);
}
