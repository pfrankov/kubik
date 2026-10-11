#include "status_json.h"
static void byte(status_json_t *json, char value) {
    if (json->used + 1 >= json->capacity) { json->failed = true; return; }
    json->data[json->used++] = value;
    json->data[json->used] = 0;
}
static void text(status_json_t *json, const char *value) {
    while (*value) byte(json, *value++);
}
static void quoted(status_json_t *json, const char *value) {
    static const char hex[] = "0123456789abcdef";
    byte(json, '"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') { byte(json, '\\'); byte(json, (char)*p); }
        else if (*p < 32) {
            text(json, "\\u00"); byte(json, hex[*p >> 4]); byte(json, hex[*p & 15]);
        } else byte(json, (char)*p);
    }
    byte(json, '"');
}
static void key(status_json_t *json, const char *name) {
    if (!json->first) byte(json, ',');
    json->first = false;
    quoted(json, name); byte(json, ':');
}
void status_json_begin(status_json_t *json, char *data, size_t capacity) {
    *json = (status_json_t){.data = data, .capacity = capacity, .first = true};
    if (capacity) data[0] = 0;
    byte(json, '{');
}
void status_json_string(status_json_t *json, const char *name, const char *value) {
    key(json, name); quoted(json, value);
}
void status_json_integer(status_json_t *json, const char *name, int64_t value) {
    char digits[20];
    size_t count = 0;
    uint64_t magnitude = value < 0 ? 0 - (uint64_t)value : (uint64_t)value;
    do { digits[count++] = (char)('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
    key(json, name);
    if (value < 0) byte(json, '-');
    while (count) byte(json, digits[--count]);
}
void status_json_bool(status_json_t *json, const char *name, bool value) {
    key(json, name); text(json, value ? "true" : "false");
}
bool status_json_end(status_json_t *json) {
    byte(json, '}');
    if (json->failed && json->capacity) json->data[0] = 0;
    return !json->failed;
}
