#include "printer_image_parse.h"
#include "link.h"
#include <math.h>
#include <string.h>

static bool valid_raw(const char *raw, size_t len, size_t consumed) {
    if (!raw || !len || len > LINK_JSON_MAX || consumed > len || memchr(raw, 0, len)) return false;
    for (size_t i = consumed; i < len; ++i) if (!strchr(" \t\r\n", raw[i])) return false;
    for (size_t i = 0; i + 5 < len; ++i)
        if (raw[i] == '\\' && !memcmp(raw + i + 1, "u0000", 5)) return false;
    return true;
}
static unsigned fields(const cJSON *request) {
    static const char *const names[] = {"t", "rid", "action", "head_width", "width", "height"};
    unsigned seen = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, request) {
        unsigned i = 0;
        while (i < 6 && item->string && strcmp(item->string, names[i])) ++i;
        if (i == 6 || !item->string || (seen & (1u << i))) return 0;
        seen |= 1u << i;
    }
    return seen;
}
static bool integer(const cJSON *request, const char *name, unsigned min, unsigned max, uint16_t *out) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request, name);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < min ||
        value->valuedouble > max || floor(value->valuedouble) != value->valuedouble) return false;
    *out = (uint16_t)value->valuedouble;
    return true;
}
static bool start_fields(const cJSON *request, printer_input_t *input) {
    uint16_t head;
    if (!integer(request, "head_width", 576, 576, &head) ||
        !integer(request, "width", 1, head, &input->image_width) ||
        !integer(request, "height", 1, 3543, &input->image_height)) return false;
    input->printing = input->image = true;
    return true;
}
printer_image_action_t printer_image_parse(cJSON *request, const char *raw, size_t len,
                                           size_t consumed, printer_input_t *input) {
    if (!request || !input || !cJSON_IsObject(request) || !valid_raw(raw, len, consumed)) return PRINTER_IMAGE_INVALID;
    memset(input, 0, sizeof *input);
    input->rid = printer_parse_id(request);
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "action"));
    unsigned seen = fields(request);
    if (!input->rid || !action) return PRINTER_IMAGE_INVALID;
    if (!strcmp(action, "abort") && seen == 7u) return PRINTER_IMAGE_ABORT;
    if (strcmp(action, "start") || seen != 63u) return PRINTER_IMAGE_INVALID;
    return start_fields(request, input) ? PRINTER_IMAGE_START : PRINTER_IMAGE_INVALID;
}
