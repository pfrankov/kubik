#include "printer_protocol.h"
#include "printer_parse.h"
#include "printer_work.h"

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

bool printer_protocol_receive(cJSON *request, const char *raw, size_t len, size_t consumed, uint32_t session) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "t"));
    if (!type || strcmp(type, "printer")) return false;
    const char *rid = printer_parse_id(request);
    if (!session || !rid) return true;
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
    char *json; uint32_t session;
    if (printer_work_poll(&json, &session)) {
        if (json && session == link_session()) link_send_json_in_session(json, session);
        free(json);
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
