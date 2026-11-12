#include "screen_image.h"
#include "agent_protocol.h"
#include "app.h"
#include "link.h"
#include "printer_parse.h"
#include "printer_preview.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

typedef struct { char rid[33]; uint32_t session; int64_t deadline; const char *error; } image_reply_t;
static image_reply_t pending;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static bool raw_valid(const char *raw, size_t len, size_t consumed) {
    if (!raw || !len || len > LINK_JSON_MAX || consumed > len || memchr(raw, 0, len) ||
        agent_protocol_has_nul_escape(raw, len)) return false;
    for (size_t i = consumed; i < len; i++)
        if (raw[i] != ' ' && raw[i] != '\r' && raw[i] != '\n' && raw[i] != '\t') return false;
    return true;
}
static unsigned field_bit(const char *key) {
    if (!key) return 0;
    if (!strcmp(key, "t")) return 1;
    if (!strcmp(key, "action")) return 2;
    if (!strcmp(key, "rid")) return 4;
    return 0;
}
static bool fields_valid(const cJSON *request) {
    unsigned seen = 0;
    for (const cJSON *field = request->child; field; field = field->next) {
        unsigned bit = field_bit(field->string);
        if (!bit || (seen & bit)) return false;
        seen |= bit;
    }
    return seen == 7;
}
static void cancel(uint32_t session, const char *rid) {
    portENTER_CRITICAL(&lock);
    if (pending.session == session && !strcmp(pending.rid, rid)) pending.session = 0;
    portEXIT_CRITICAL(&lock);
    printer_preview_cancel_screen(session, rid);
}
static bool claim(uint32_t session, const char *rid, bool valid) {
    int64_t deadline = esp_timer_get_time() + 4500000;
    portENTER_CRITICAL(&lock);
    if (pending.session && pending.session != session) pending.session = 0;
    bool busy = pending.session != 0;
    if (!busy) {
        memcpy(pending.rid, rid, sizeof pending.rid);
        pending.session = session;
        pending.deadline = deadline;
        pending.error = valid ? NULL : "invalid_request";
    }
    portEXIT_CRITICAL(&lock);
    return !busy;
}
static void unavailable(uint32_t session, const char *rid) {
    portENTER_CRITICAL(&lock);
    if (pending.session == session && !strcmp(pending.rid, rid)) pending.error = "unavailable";
    portEXIT_CRITICAL(&lock);
}

static bool is_action(const char *value, const char *expected) { return value && !strcmp(value, expected); }
bool screen_image_receive(const cJSON *request, const char *raw, size_t len, size_t consumed, uint32_t session) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "t"));
    if (!type || strcmp(type, "screen_image")) return false;
    const char *rid = printer_parse_id(request);
    if (!rid || !session) return true;
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "action"));
    bool valid = raw_valid(raw, len, consumed) && fields_valid(request);
    if (valid && is_action(action, "cancel")) { cancel(session, rid); return true; }
    valid = valid && is_action(action, "show");
    if (!claim(session, rid, valid)) return true;
    if (valid && printer_preview_activate_screen(session, rid))
        app_post_in_session(EV_SRV_PREVIEW, 0, 0, session);
    else if (valid) unavailable(session, rid);
    return true;
}

static void release_reply(const image_reply_t *reply) {
    portENTER_CRITICAL(&lock);
    if (pending.session == reply->session && !strcmp(pending.rid, reply->rid)) pending.session = 0;
    portEXIT_CRITICAL(&lock);
}
static bool send_reply(const image_reply_t *reply, bool shown, const char *error) {
    char json[160];
    if (shown) snprintf(json, sizeof json, "{\"t\":\"screen_image_result\",\"rid\":\"%s\",\"status\":\"shown\"}", reply->rid);
    else snprintf(json, sizeof json, "{\"t\":\"screen_image_result\",\"rid\":\"%s\",\"status\":\"error\",\"code\":\"%s\"}", reply->rid, error);
    return link_send_json_in_session(json, reply->session);
}
void screen_image_poll(void) {
    image_reply_t reply;
    portENTER_CRITICAL(&lock);
    reply = pending;
    portEXIT_CRITICAL(&lock);
    if (!reply.session) return;
    if (reply.session != link_session()) { release_reply(&reply); return; }
    bool shown = !reply.error && printer_preview_screen_presented(reply.session, reply.rid);
    const char *error = reply.error;
    if (!shown && !error && esp_timer_get_time() >= reply.deadline) error = "timeout";
    if (!shown && !error) return;
    if (!send_reply(&reply, shown, error)) {
        if (esp_timer_get_time() >= reply.deadline + 1000000) {
            release_reply(&reply);
            printer_preview_cancel_screen(reply.session, reply.rid);
        }
        return;
    }
    release_reply(&reply);
    if (!shown) printer_preview_cancel_screen(reply.session, reply.rid);
}
