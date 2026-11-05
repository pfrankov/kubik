#include "printer_protocol.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "link.h"

typedef struct { char rid[33]; uint32_t session; bool invalid; } printer_reply_t;
static printer_reply_t s_replies[PRINTER_REPLY_SLOTS];
static unsigned s_head, s_count;
static uint32_t s_session;
static portMUX_TYPE s_reply_mux = portMUX_INITIALIZER_UNLOCKED;

static bool lower_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static bool request_id(const cJSON *item) {
    if (!cJSON_IsString(item) || !item->valuestring || strlen(item->valuestring) != 32) return false;
    for (unsigned i = 0; i < 32; i++) if (!lower_hex((unsigned char)item->valuestring[i])) return false;
    return true;
}

static bool raw_string_invalid(const char *raw, size_t len) {
    if (!raw || !len || len > LINK_JSON_MAX || memchr(raw, 0, len)) return true;
    bool quoted = false;
    for (size_t i = 0; i < len; i++) {
        if (raw[i] == '"') quoted = !quoted;
        if (quoted && (unsigned char)raw[i] < 0x20) return true;
        if (quoted && raw[i] == '\\') {
            if (i + 5 < len && !memcmp(raw + i + 1, "u0000", 5)) return true;
            i++; // Literal escaped backslashes do not introduce a Unicode NUL.
        }
    }
    return false;
}

static bool scalar_valid(uint32_t cp, uint32_t minimum) {
    return cp >= minimum && cp <= 0x10ffff && (cp < 0xd800 || cp > 0xdfff);
}

static bool next_codepoint(const unsigned char **cursor, uint32_t *codepoint) {
    const unsigned char *p = *cursor;
    unsigned char first = *p++;
    unsigned tail;
    uint32_t minimum;
    if (first < 0x80) { *codepoint = first; *cursor = p; return true; }
    if (first >= 0xc2 && first <= 0xdf) { tail = 1; minimum = 0x80; *codepoint = first & 0x1f; }
    else if (first >= 0xe0 && first <= 0xef) { tail = 2; minimum = 0x800; *codepoint = first & 0x0f; }
    else if (first >= 0xf0 && first <= 0xf4) { tail = 3; minimum = 0x10000; *codepoint = first & 0x07; }
    else return false;
    for (unsigned i = 0; i < tail; i++) {
        if ((*p & 0xc0) != 0x80) return false;
        *codepoint = (*codepoint << 6) | (*p++ & 0x3f);
    }
    if (!scalar_valid(*codepoint, minimum)) return false;
    *cursor = p;
    return true;
}

static bool whitespace(uint32_t cp) {
    static const uint32_t ranges[][2] = {{9,13},{32,32},{0x85,0x85},{0xa0,0xa0},{0x1680,0x1680},
        {0x2000,0x200a},{0x2028,0x2029},{0x202f,0x202f},{0x205f,0x205f},{0x3000,0x3000},{0xfeff,0xfeff}};
    for (unsigned i = 0; i < sizeof ranges / sizeof ranges[0]; i++)
        if (cp >= ranges[i][0] && cp <= ranges[i][1]) return true;
    return false;
}

static bool forbidden_control(uint32_t cp) {
    return (cp < 0x20 && cp != '\n' && cp != '\t') || (cp >= 0x7f && cp <= 0x9f);
}

static bool printable_text(const cJSON *item) {
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    size_t len = strnlen(item->valuestring, PRINTER_TEXT_MAX_BYTES + 1);
    if (!len || len > PRINTER_TEXT_MAX_BYTES) return false;
    const unsigned char *p = (const unsigned char *)item->valuestring;
    bool visible = false;
    while (*p) {
        uint32_t cp;
        if (!next_codepoint(&p, &cp) || forbidden_control(cp)) return false;
        visible |= !whitespace(cp);
    }
    return visible;
}

static bool request_fields(cJSON *request) {
    static const char *const names[] = {"t", "rid", "action", "text"};
    unsigned seen = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, request) {
        unsigned i = 0;
        while (i < 4 && strcmp(item->string, names[i])) i++;
        if (i == 4 || (seen & (1u << i))) return false;
        seen |= 1u << i;
    }
    return (seen & 7u) == 7u;
}

static bool complete_json(const char *raw, size_t len, size_t consumed) {
    if (!raw || consumed > len) return false;
    for (size_t i = consumed; i < len; i++) if (!strchr(" \t\r\n", raw[i])) return false;
    return true;
}

static bool valid_request(cJSON *request, const char *raw, size_t len, size_t consumed) {
    if (raw_string_invalid(raw, len) || !complete_json(raw, len, consumed) || !request_fields(request)) return false;
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "action"));
    cJSON *text = cJSON_GetObjectItemCaseSensitive(request, "text");
    if (!action) return false;
    if (!strcmp(action, "status")) return !text;
    return !strcmp(action, "print") && printable_text(text);
}

static void enqueue(const char *rid, uint32_t session, bool invalid) {
    portENTER_CRITICAL(&s_reply_mux);
    if (s_session != session) { s_session = session; s_head = s_count = 0; }
    bool duplicate = false;
    for (unsigned i = 0; i < s_count; i++)
        duplicate |= !strcmp(s_replies[(s_head + i) % PRINTER_REPLY_SLOTS].rid, rid);
    if (!duplicate && s_count < PRINTER_REPLY_SLOTS) {
        printer_reply_t *reply = &s_replies[(s_head + s_count++) % PRINTER_REPLY_SLOTS];
        memcpy(reply->rid, rid, sizeof reply->rid);
        reply->session = session;
        reply->invalid = invalid;
    }
    portEXIT_CRITICAL(&s_reply_mux);
}

bool printer_protocol_receive(cJSON *request, const char *raw, size_t len, size_t consumed, uint32_t session) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "t"));
    if (!type || strcmp(type, "printer")) return false;
    cJSON *rid = cJSON_GetObjectItemCaseSensitive(request, "rid");
    if (session && request_id(rid)) enqueue(rid->valuestring, session, !valid_request(request, raw, len, consumed));
    return true;
}

void printer_protocol_poll(void) {
    printer_reply_t reply;
    portENTER_CRITICAL(&s_reply_mux);
    bool pending = s_count > 0;
    if (pending) { reply = s_replies[s_head]; s_head = (s_head + 1) % PRINTER_REPLY_SLOTS; s_count--; }
    portEXIT_CRITICAL(&s_reply_mux);
    if (!pending || reply.session != link_session()) return;
    char json[144];
    // No physical driver is configured. Receiving text never means that paper was printed.
    const char *result = reply.invalid ? "\"error\",\"code\":\"invalid_request\"" : "\"not_configured\"";
    snprintf(json, sizeof json, "{\"t\":\"printer_result\",\"rid\":\"%s\",\"status\":%s}", reply.rid, result);
    link_send_json_in_session(json, reply.session);
}
