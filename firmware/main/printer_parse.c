#include "printer_parse.h"
#include "printer_emoji_parse.h"

#include <math.h>
#include <string.h>
#include "link.h"

const char *printer_parse_id(const cJSON *request) {
    const char *rid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "rid"));
    if (!rid || strlen(rid) != 32) return NULL;
    for (unsigned i = 0; i < 32; i++) {
        unsigned char c = rid[i];
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return NULL;
    }
    return rid;
}

static bool bounds_invalid(const char *raw, size_t len, size_t consumed) {
    if (!raw || !len || len > LINK_JSON_MAX || consumed > len || memchr(raw, 0, len)) return true;
    for (size_t i = consumed; i < len; i++) if (!strchr(" \t\r\n", raw[i])) return true;
    return false;
}

static bool raw_invalid(const char *raw, size_t len, size_t consumed) {
    if (bounds_invalid(raw, len, consumed)) return true;
    bool quoted = false;
    for (size_t i = 0; i < len; i++) {
        if (raw[i] == '"') quoted = !quoted;
        if (quoted && (unsigned char)raw[i] < 0x20) return true;
        if (quoted && raw[i] == '\\') {
            if (i + 5 < len && !memcmp(raw + i + 1, "u0000", 5)) return true;
            i++;
        }
    }
    return false;
}

static bool fields(cJSON *request, unsigned *seen) {
    static const char *const names[] = {"t", "rid", "action", "text", "format", "width_mm", "height_mm", "font_mm", "emoji"};
    *seen = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, request) {
        unsigned i = 0;
        while (i < 9 && strcmp(item->string, names[i])) i++;
        if (i == 9 || (*seen & (1u << i))) return false;
        *seen |= 1u << i;
    }
    return (*seen & 7u) == 7u;
}

static bool number(cJSON *request, const char *key, double *value) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(request, key);
    *value = 0;
    if (!item) return true;
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) return false;
    *value = item->valuedouble;
    return *value > 0;
}

static bool document(cJSON *request, printer_document_t *doc) {
    memset(doc, 0, sizeof *doc);
    doc->text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "text"));
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(request, "format");
    if (format) {
        const char *name = cJSON_GetStringValue(format);
        if (!name) return false;
        if (!strcmp(name, "qr")) doc->format = PRINTER_FORMAT_QR;
        else if (strcmp(name, "text")) return false;
    }
    return number(request, "width_mm", &doc->width_mm) && number(request, "height_mm", &doc->height_mm) &&
        number(request, "font_mm", &doc->font_mm) && printer_document_valid(doc);
}

static bool print_request(cJSON *request, printer_input_t *input, unsigned seen) {
    if (!(seen & 8u) || !document(request,&input->document)) return false;
    input->emoji=cJSON_GetObjectItemCaseSensitive(request,"emoji");
    if (input->document.format==PRINTER_FORMAT_QR) return !input->emoji;
    return printer_emoji_parse_valid(input->emoji,input->document.text,&input->emoji_count);
}

bool printer_parse_request(cJSON *request, const char *raw, size_t len,
                           size_t consumed, printer_input_t *input) {
    memset(input, 0, sizeof *input);
    input->rid = printer_parse_id(request);
    unsigned seen;
    if (!input->rid || !cJSON_IsObject(request) || raw_invalid(raw, len, consumed) || !fields(request, &seen)) return false;
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "action"));
    if (!action) return false;
    input->printing = !strcmp(action, "print");
    if (input->printing) return print_request(request,input,seen);
    return !strcmp(action, "status") && seen == 7u;
}
