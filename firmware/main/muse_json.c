#include "muse_json.h"
#include "muse_store.h"
#include <stdbool.h>
#include <string.h>

static bool escaped_null(const char *bytes, size_t size, size_t offset) {
    return bytes[offset] == 'u' && offset + 4 < size && !memcmp(bytes + offset + 1, "0000", 4);
}
static bool depth_step(char value, unsigned *depth) {
    if (value == '{' || value == '[') return ++*depth <= 16;
    if (value == '}' || value == ']') { if (!*depth) return false; --*depth; }
    return true;
}
static bool bounded(const char *bytes, size_t size) {
    unsigned depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < size; i++) {
        char value = bytes[i];
        if (quoted && value == '\\') {
            if (++i >= size) return false;
            if (escaped_null(bytes, size, i)) return false;
        } else if (value == '"') quoted = !quoted;
        else if (!quoted && !depth_step(value, &depth)) return false;
    }
    return !quoted && !depth;
}

cJSON *muse_json_parse(const char *bytes, size_t size) {
    if (!bytes || !size || size > 16384 || memchr(bytes, 0, size) || !bounded(bytes, size)) return NULL;
    const char *end = NULL;
    cJSON *object = cJSON_ParseWithLengthOpts(bytes, size, &end, false);
    if (!object) return NULL;
    while (end < bytes + size && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) end++;
    if (end != bytes + size) { muse_json_clear(object); return NULL; }
    return object;
}
static void wipe_strings(cJSON *object) {
    for (cJSON *item = object; item; item = item->next) {
        if (item->valuestring) muse_store_wipe(item->valuestring, strlen(item->valuestring));
        if (item->child) wipe_strings(item->child);
    }
}
void muse_json_clear(cJSON *object) { wipe_strings(object); cJSON_Delete(object); }
