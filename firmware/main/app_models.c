#include "app_models.h"
#include "app_internal.h"
#include "agent_protocol.h"
#include <stdio.h>

static void add_agent_models(cJSON *info, unsigned offset) {
    cJSON *models = cJSON_CreateArray();
    face_lock();
    cJSON_AddStringToObject(info, "agent_target", agent_target_name(g_face.agent.target));
    cJSON_AddStringToObject(info, "voice_model", g_face.agent.selected_model);
    unsigned end = offset + 16;
    cJSON_AddNumberToObject(info, "models_next", end < g_face.agent.count ? (int)end : -1);
    for (unsigned i = offset; models && i < g_face.agent.count && i < end; i++)
        cJSON_AddItemToArray(models, cJSON_CreateString(g_face.agent.models[i].id));
    face_unlock();
    if (models && !cJSON_AddItemToObject(info, "agent_models", models)) cJSON_Delete(models);
}
void app_config_models(cJSON *request, char *reply, size_t cap) {
    cJSON *cursor = cJSON_GetObjectItemCaseSensitive(request, "offset");
    unsigned offset = cursor ? (unsigned)cursor->valueint : 0;
    if (cursor && (!cJSON_IsNumber(cursor) || cursor->valuedouble != offset || (offset != 0 && offset != 16))) {
        snprintf(reply, cap, "{\"ok\":false,\"error\":\"invalid offset\"}"); return;
    }
    cJSON *info = cJSON_CreateObject();
    bool ok = info && cJSON_AddBoolToObject(info, "ok", true);
    if (ok) add_agent_models(info, offset);
    if (!ok || !cJSON_PrintPreallocated(info, reply, (int)cap, false)) snprintf(reply, cap, "{\"ok\":false,\"error\":\"models unavailable\"}");
    cJSON_Delete(info);
}
