#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// One bounded status object, written directly into the caller's reply buffer.
typedef struct { char *data; size_t capacity, used; bool failed, first; } status_json_t;
void status_json_begin(status_json_t *json, char *data, size_t capacity);
void status_json_string(status_json_t *json, const char *key, const char *value);
void status_json_integer(status_json_t *json, const char *key, int64_t value);
void status_json_bool(status_json_t *json, const char *key, bool value);
bool status_json_end(status_json_t *json);
