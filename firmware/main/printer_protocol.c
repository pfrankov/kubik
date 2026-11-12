#include "printer_protocol.h"
#include "printer_parse.h"
#include "printer_image_parse.h"
#include "printer_work.h"
#include "printer_preview.h"
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "link.h"

typedef struct { char rid[33]; uint32_t session; const char *error; bool printing; } printer_reply_t;
static printer_reply_t replies[PRINTER_REPLY_SLOTS];
static unsigned head, count;
static uint32_t reply_session;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static bool already_queued(const char *rid) {
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(replies[(head + i) % PRINTER_REPLY_SLOTS].rid, rid)) return true;
    return false;
}

static void enqueue(const char *rid, uint32_t session, bool printing, const char *error) {
    portENTER_CRITICAL(&lock);
    if (reply_session != session) { reply_session = session; head = count = 0; }
    if (!already_queued(rid) && count < PRINTER_REPLY_SLOTS) {
        printer_reply_t *reply = &replies[(head + count++) % PRINTER_REPLY_SLOTS];
        memcpy(reply->rid, rid, sizeof reply->rid);
        reply->session = session; reply->printing = printing; reply->error = error;
    }
    portEXIT_CRITICAL(&lock);
}

static void receive_image(cJSON *request, const char *raw, size_t len, size_t consumed,
                          uint32_t session, const char *rid) {
    printer_input_t image;
    printer_image_action_t action = printer_image_parse(request, raw, len, consumed, &image);
    if (action == PRINTER_IMAGE_ABORT) {
        printer_preview_discard(session, rid);
        printer_work_image_abort(session, rid); return;
    }
    if (action == PRINTER_IMAGE_INVALID) {
        printer_preview_discard(session, rid);
        enqueue(rid, session, true, "invalid_request"); return;
    }
    if (printer_work_has_id(rid)) { printer_preview_discard(session, rid); return; }
    printer_admission_t admitted = printer_work_submit(&image, session);
    if (admitted == PRINTER_ADMITTED && printer_preview_activate(session, rid))
        app_post_in_session(EV_SRV_PREVIEW, 0, 0, session);
    if (admitted == PRINTER_WORK_BUSY || admitted == PRINTER_WORK_FAILED)
        printer_preview_discard(session, rid);
    if (admitted == PRINTER_WORK_BUSY) enqueue(rid, session, true, "printer_busy");
    if (admitted == PRINTER_WORK_FAILED) enqueue(rid, session, true, "printer_failed");
}

bool printer_protocol_receive(cJSON *request, const char *raw, size_t len, size_t consumed, uint32_t session) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "t"));
    if (!type || (strcmp(type, "printer") && strcmp(type, "printer_image"))) return false;
    const char *rid = printer_parse_id(request);
    if (!session || !rid) return true;
    if (!strcmp(type, "printer_image")) { receive_image(request, raw, len, consumed, session, rid); return true; }
    if (printer_work_has_id(rid)) return true;
    printer_input_t input;
    if (!printer_parse_request(request, raw, len, consumed, &input)) {
        const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "action"));
        enqueue(rid, session, action && !strcmp(action, "print"), "invalid_request");
        return true;
    }
    printer_admission_t admitted = printer_work_submit(&input, session);
    if (admitted == PRINTER_WORK_BUSY) enqueue(rid, session, input.printing, "printer_busy");
    if (admitted == PRINTER_WORK_FAILED) enqueue(rid, session, input.printing, "printer_failed");
    return true;
}

void printer_protocol_image_receive(uint8_t tag, const uint8_t *data, size_t len, uint32_t session) {
    if (!tag && session) printer_work_image_receive(session, data, len);
}

static void send_error(void) {
    printer_reply_t reply;
    portENTER_CRITICAL(&lock);
    bool pending = count > 0;
    if (pending) { reply = replies[head]; head = (head + 1) % PRINTER_REPLY_SLOTS; count--; }
    portEXIT_CRITICAL(&lock);
    if (!pending || reply.session != link_session()) return;
    char json[192];
    snprintf(json, sizeof json, "{\"t\":\"printer_result\",\"rid\":\"%s\",\"status\":\"error\",\"code\":\"%s\"%s}",
        reply.rid, reply.error, reply.printing ? ",\"outcome\":\"not_sent\"" : "");
    link_send_json_in_session(json, reply.session);
}

void printer_protocol_poll(void) {
    printer_preview_t *preview = printer_preview_snapshot(link_session());
    printer_preview_release(preview);
    char *json; uint32_t session;
    if (printer_work_poll(&json, &session)) {
        printer_preview_clear(session);
        if (json && session == link_session()) link_send_json_in_session(json, session);
        free(json);
        return;
    }
    printer_stream_credit_t credit;
    if (printer_work_image_credit(&credit)) {
        char json[160];
        snprintf(json, sizeof json, "{\"t\":\"printer_image_ready\",\"rid\":\"%s\",\"offset\":%u}",
                 credit.rid, credit.offset);
        if (!link_send_json_in_session(json, credit.session)) printer_work_image_abort(credit.session, credit.rid);
        return;
    }
    send_error();
}

bool printer_protocol_busy(void) { return printer_work_busy(); }
bool printer_protocol_discovered(void) { return printer_work_discovered(); }
bool printer_protocol_before_voice(void) { return printer_work_before_voice(); }
bool printer_protocol_reset_ready(void) {
    printer_work_before_voice();
    return !printer_work_busy();
}

void printer_protocol_info(status_json_t *info) {
    printer_diagnostics_t snapshot;
    printer_work_diagnostics(&snapshot);
    printer_preview_t *preview = printer_preview_snapshot(link_session());
    status_json_bool(info, "printer_preview_active", preview != NULL);
    status_json_bool(info, "printer_preview_presented", printer_preview_was_presented(preview));
    printer_preview_release(preview);
    status_json_bool(info, "printer_busy", printer_work_busy());
    status_json_bool(info, "printer_discovered", printer_work_discovered());
    status_json_integer(info, "printer_session", snapshot.state.session);
    status_json_integer(info, "printer_observed_session", snapshot.observed_session);
    status_json_bool(info, "printer_enabled", snapshot.state.enabled);
    status_json_bool(info, "printer_online", snapshot.state.online);
    status_json_bool(info, "printer_can_start", snapshot.state.can_start);
    status_json_bool(info, "printer_auto_allowed", snapshot.state.automatic_allowed);
    status_json_integer(info, "printer_blockers", snapshot.state.blockers);
    status_json_bool(info, "printer_guard_allowed", snapshot.guard_allowed);
    status_json_integer(info, "printer_attempts", snapshot.attempts);
    status_json_integer(info, "printer_due_ms", snapshot.due_in_ms);
    status_json_integer(info, "reset_reason", printer_platform_reset_reason());
}
