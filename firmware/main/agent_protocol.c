#include "agent_protocol.h"

#include <string.h>

static bool string_copy(cJSON *object, const char *key, char *out, size_t cap, bool required) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) {
        if (cap) out[0] = 0;
        return !required;
    }
    if (!cJSON_IsString(item) || !item->valuestring || !cap) return false;
    size_t len = strlen(item->valuestring);
    if (len >= cap) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)item->valuestring[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    memcpy(out, item->valuestring, len + 1);
    return true;
}

static bool uint_value(cJSON *object, const char *key, unsigned max, unsigned *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > max ||
        item->valuedouble != (double)item->valueint) return false;
    *out = (unsigned)item->valueint;
    return true;
}

static bool parse_model(cJSON *entry, agent_model_t *model) {
    cJSON *available = cJSON_GetObjectItemCaseSensitive(entry, "available");
    if (available && !cJSON_IsBool(available)) return false;
    model->disabled = available && cJSON_IsFalse(available);
    return cJSON_IsObject(entry) && string_copy(entry, "id", model->id, sizeof model->id, true) &&
        string_copy(entry, "label", model->label, sizeof model->label, true) && model->label[0] &&
        agent_menu_id_valid(model->id);
}

static bool parse_catalog(cJSON *json, agent_menu_reply_t *reply) {
    cJSON *models = cJSON_GetObjectItemCaseSensitive(json, "models");
    if (!cJSON_IsArray(models) || cJSON_GetArraySize(models) > AGENT_PAGE_SIZE) return false;
    reply->count = (uint8_t)cJSON_GetArraySize(models);
    for (unsigned i = 0; i < reply->count; i++)
        if (!parse_model(cJSON_GetArrayItem(models, (int)i), &reply->models[i])) return false;
    reply->has_catalog = true;
    return true;
}

static bool parse_status(cJSON *json, const char *key, bool *available, char *provider, char *model) {
    cJSON *status = cJSON_GetObjectItemCaseSensitive(json, key);
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(status, "available");
    if (!cJSON_IsObject(status) || !cJSON_IsBool(enabled)) return false;
    *available = cJSON_IsTrue(enabled);
    return string_copy(status, "provider", provider, AGENT_META_MAX + 1, false) &&
        string_copy(status, "model", model, AGENT_META_MAX + 1, false);
}

static bool parse_reply_header(cJSON *json, agent_menu_reply_t *reply) {
    unsigned rid, cursor, total;
    char target[6];
    if (!string_copy(json, "target", target, sizeof target, true) || !agent_target_parse(target, &reply->target)) return false;
    if (!uint_value(json, "rid", UINT16_MAX, &rid) || !uint_value(json, "cursor", UINT8_MAX, &cursor) ||
        !uint_value(json, "total", 128, &total)) return false;
    reply->rid = (uint16_t)rid;
    reply->cursor = (uint8_t)cursor;
    reply->total = (uint8_t)total;
    if (!string_copy(json, "error", reply->error, sizeof reply->error, false) ||
        !agent_menu_error_valid(reply->error)) return false;
    return true;
}

static bool parse_reply_data(cJSON *json, agent_menu_reply_t *reply) {
    cJSON *current = cJSON_GetObjectItemCaseSensitive(json, "model");
    if (current) {
        if (!string_copy(json, "model", reply->model, sizeof reply->model, true) ||
            (reply->model[0] && !agent_menu_id_valid(reply->model))) return false;
        reply->has_model = true;
    }
    reply->has_stt = parse_status(json, "stt", &reply->stt_available, reply->stt_provider, reply->stt_model);
    reply->has_tts = parse_status(json, "tts", &reply->tts_available, reply->tts_provider, reply->tts_model);
    if (reply->error[0]) return true;
    return reply->has_model && parse_catalog(json, reply) && reply->has_stt && reply->has_tts;
}

bool agent_protocol_parse_options(cJSON *json, agent_menu_reply_t *reply) {
    if (!cJSON_IsObject(json) || !reply) return false;
    *reply = (agent_menu_reply_t){0};
    return parse_reply_header(json, reply) && parse_reply_data(json, reply);
}

bool agent_protocol_parse_capabilities(cJSON *json, bool *stt, bool *tts) {
    if (!cJSON_IsObject(json) || !stt || !tts) return false;
    cJSON *stt_status = cJSON_GetObjectItemCaseSensitive(json, "stt");
    cJSON *tts_status = cJSON_GetObjectItemCaseSensitive(json, "tts");
    cJSON *stt_available = cJSON_GetObjectItemCaseSensitive(stt_status, "available");
    cJSON *tts_available = cJSON_GetObjectItemCaseSensitive(tts_status, "available");
    if (!cJSON_IsObject(stt_status) || !cJSON_IsObject(tts_status) ||
        !cJSON_IsBool(stt_available) || !cJSON_IsBool(tts_available)) return false;
    *stt = cJSON_IsTrue(stt_available);
    *tts = cJSON_IsTrue(tts_available);
    return true;
}

bool agent_protocol_has_nul_escape(const char *json, size_t len) {
    if (!json) return false;
    for (size_t i = 0; i + 5 < len; i++)
        if (json[i] == '\\' && !memcmp(json + i + 1, "u0000", 5)) return true;
    return false;
}
