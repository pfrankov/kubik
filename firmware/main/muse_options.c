#include "muse_options.h"
#include <string.h>

static void status(cJSON *reply, const char *key, bool available) {
    cJSON *object = cJSON_AddObjectToObject(reply, key);
    cJSON_AddBoolToObject(object, "available", available);
    cJSON_AddStringToObject(object, "provider", available ? "Muse" : "");
    cJSON_AddStringToObject(object, "model", available ? "Muse" : "");
}
static void model(cJSON *models, const char *id, const char *label, bool available) {
    cJSON *object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "id", id); cJSON_AddStringToObject(object, "label", label);
    cJSON_AddBoolToObject(object, "available", available); cJSON_AddItemToArray(models, object);
}
static bool selection_unsupported(const cJSON *request, bool mode) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "t"));
    const char *selection = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "id"));
    return type && !strcmp(type, "agent_model") && (!mode || !selection || strcmp(selection, "classic"));
}
cJSON *muse_options_reply(const cJSON *request, bool available) {
    const char *target = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "target"));
    cJSON *rid = cJSON_GetObjectItemCaseSensitive(request, "rid");
    if (!target || !cJSON_IsNumber(rid)) return NULL;
    bool mode = !strcmp(target, "mode"), stt = !strcmp(target, "stt"), agent = !strcmp(target, "agent");
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "t", "agent_options");
    cJSON_AddStringToObject(reply, "target", target); cJSON_AddNumberToObject(reply, "rid", rid->valueint);
    cJSON_AddNumberToObject(reply, "cursor", 0);
    cJSON_AddStringToObject(reply, "model", mode ? "classic" : stt || agent ? "muse" : "");
    cJSON *models = cJSON_AddArrayToObject(reply, "models");
    if (mode) {
        model(models, "classic", "STT", true);
        model(models, "realtime", "Realtime", false); model(models, "live", "GPT Live", false);
    } else if (stt || agent) model(models, "muse", "Managed by Muse", false);
    cJSON_AddNumberToObject(reply, "total", cJSON_GetArraySize(models));
    status(reply, "stt", available); status(reply, "tts", false);
    if (selection_unsupported(request, mode))
        cJSON_AddStringToObject(reply, "error", "unsupported");
    return reply;
}
