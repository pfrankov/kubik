#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "agent_protocol.h"
#include "link_usb.h"
static struct { agent_menu_t agent; } g_face;
static void face_lock(void) {} static void face_unlock(void) {}
/* PRODUCTION_MODELS */
int main(void) {
    g_face.agent.count = AGENT_LIST_MAX;
    for (unsigned i = 0; i < AGENT_LIST_MAX; i++) {
        memset(g_face.agent.models[i].id, i % 2 ? '\\' : '"', AGENT_MODEL_ID_MAX);
        g_face.agent.models[i].id[AGENT_MODEL_ID_MAX] = 0;
    }
    strcpy(g_face.agent.selected_model, g_face.agent.models[0].id);
    char reply[LINK_USB_CONFIG_REPLY_CAP]; cJSON *request = cJSON_CreateObject();
    app_config_models(request, reply, sizeof reply);
    cJSON *json = cJSON_Parse(reply); assert(json && cJSON_IsTrue(cJSON_GetObjectItem(json, "ok")));
    cJSON *models = cJSON_GetObjectItem(json, "agent_models");
    assert(cJSON_GetArraySize(models) == 16);
    assert(cJSON_GetObjectItem(json, "models_next")->valueint == 16);
    for (unsigned i = 0; i < 16; i++)
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(models, i)), g_face.agent.models[i].id));
    cJSON_Delete(json);
    cJSON_AddNumberToObject(request, "offset", 16); app_config_models(request, reply, sizeof reply);
    json = cJSON_Parse(reply); assert(json && cJSON_IsTrue(cJSON_GetObjectItem(json, "ok")));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(json, "agent_models")) == 16);
    assert(cJSON_GetObjectItem(json, "models_next")->valueint == -1);
    for (unsigned i = 0; i < 16; i++)
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetObjectItem(json, "agent_models"), i)), g_face.agent.models[i+16].id));
    cJSON_Delete(json);
    app_config_models(request, reply, 128); json = cJSON_Parse(reply);
    assert(json && cJSON_IsFalse(cJSON_GetObjectItem(json, "ok"))); cJSON_Delete(json);
    cJSON_ReplaceItemInObject(request, "offset", cJSON_CreateNumber(1));
    app_config_models(request, reply, sizeof reply); json = cJSON_Parse(reply);
    assert(json && cJSON_IsFalse(cJSON_GetObjectItem(json, "ok")));
    cJSON_Delete(json); cJSON_Delete(request);
}
