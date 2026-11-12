#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "agent_protocol.h"
#include "link_usb.h"
static struct { agent_menu_t agent; } g_face;
static void face_lock(void) {} static void face_unlock(void) {}
/* PRODUCTION_MODELS */
static void dispatch_config(cJSON *j,const char *command,char *reply,size_t cap) {
    assert(!strcmp(command,"models"));app_config_models(j,reply,cap);
}
static unsigned info_fields;
static void app_config_info_fields(status_json_t *info) { info_fields++; status_json_bool(info, "ok", true); }
/* PRODUCTION_CONFIG */
typedef struct { char data[64]; size_t used; } sink_t;
static bool write_status(void *context, char value) {
    sink_t *sink=context; assert(sink->used < sizeof sink->data); sink->data[sink->used++]=value; return true;
}
static void check_streamed_info(void) {
    sink_t sink={0}; bool ok=false;
    const char *info="{\"cmd\":\"info\"}";
    void *parsed=on_config_parse(info,strlen(info));
    assert(on_config_info(parsed,write_status,&sink,&ok) && ok && info_fields==1);
    on_config_free(parsed);
    assert(sink.used==11 && !memcmp(sink.data,"{\"ok\":true}",11));
    const char *models="{\"cmd\":\"models\"}";
    parsed=on_config_parse(models,strlen(models));
    assert(!on_config_info(parsed,write_status,&sink,&ok) && info_fields==1);
    on_config_free(parsed);
}
static void *sensitive[3];
static size_t sensitive_length[3];
static unsigned erased;
static void *json_alloc(size_t n) { return malloc(n); }
static void json_free(void *pointer) {
    for (unsigned i=0;i<3;i++) if (pointer==sensitive[i]) {
        for (size_t j=0;j<sensitive_length[i];j++) assert(((const char *)pointer)[j]==0);
        erased |= 1u<<i;
    }
    free(pointer);
}
static void check_embedded_nul_rejection(void) {
    // JSON source bytes: one and three backslashes introduce a real Unicode NUL.
    const char single[] = {'{','"','p','"',':','"','a','\\','u','0','0','0','0','b','"','}'};
    const char triple[] = {'{','"','p','"',':','"','a','\\','\\','\\','u','0','0','0','0','b','"','}'};
    const char pair[] = {'{','"','p','"',':','"','a','\\','\\','u','0','0','0','0','b','"','}'};
    const char four[] = {'{','"','p','"',':','"','a','\\','\\','\\','\\','u','0','0','0','0','b','"','}'};
    const char raw[] = {'{','"','p','"',':','"','a',0,'b','"','}'};
    assert(!on_config_parse(single,sizeof single));
    assert(!on_config_parse(triple,sizeof triple));
    assert(!on_config_parse(raw,sizeof raw));
    cJSON *item=on_config_parse(pair,sizeof pair); assert(item);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(item,"p")),"a\\u0000b"));
    on_config_free(item);
    item=on_config_parse(four,sizeof four); assert(item);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(item,"p")),"a\\\\u0000b"));
    on_config_free(item);
}
static void check_secret_scrub_on_reply_oom(void) {
    cJSON_Hooks hooks={.malloc_fn=json_alloc,.free_fn=json_free}; cJSON_InitHooks(&hooks);
    const char request[]="{\"cmd\":\"set\",\"pass\":\"secret\",\"nested\":[{\"sdk_token\":\"private-token\"}]}";
    cJSON *parsed=on_config_parse(request,strlen(request));assert(parsed);
    cJSON *pass=cJSON_GetObjectItem(parsed,"pass"), *nested=cJSON_GetObjectItem(parsed,"nested");
    cJSON *token=cJSON_GetObjectItem(cJSON_GetArrayItem(nested,0),"sdk_token");
    sensitive[0]=pass->valuestring;sensitive_length[0]=strlen(pass->valuestring);
    sensitive[1]=token->valuestring;sensitive_length[1]=strlen(token->valuestring);
    sensitive[2]=token->string;sensitive_length[2]=strlen(token->string);
    on_config_free(parsed); // Reply allocation failed: no execution took place.
    assert(erased==7);cJSON_InitHooks(NULL);
}
static void check_aliased_reply(char *reply,size_t capacity) {
    strcpy(reply,"{\"cmd\":\"models\"}");
    void *parsed=on_config_parse(reply,strlen(reply));
    memset(reply,0,capacity); // The parsed tree owns all strings after RX is retired.
    on_config_reply(parsed,reply,capacity);
    on_config_free(parsed);
    cJSON *json=cJSON_Parse(reply);assert(json && cJSON_IsTrue(cJSON_GetObjectItem(json,"ok")));
    cJSON *models=cJSON_GetObjectItem(json,"agent_models");assert(cJSON_GetArraySize(models)==16);
    for(unsigned i=0;i<16;i++)assert(!strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(models,i)),g_face.agent.models[i].id));
    cJSON_Delete(json);
}
int main(void) {
    check_streamed_info();
    check_embedded_nul_rejection();
    check_secret_scrub_on_reply_oom();
    assert(agent_menu_reserve_catalog(&g_face.agent));
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
    check_aliased_reply(reply, sizeof reply);
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
    agent_menu_hide(&g_face.agent);
    assert(g_face.agent.models && g_face.agent.count == AGENT_LIST_MAX);
    agent_menu_visit(&g_face.agent, AGENT_VIEW_OVERVIEW, AGENT_TARGET_AGENT);
    assert(!g_face.agent.models && !g_face.agent.count);
    request = cJSON_CreateObject(); app_config_models(request, reply, sizeof reply);
    json = cJSON_Parse(reply); assert(json && cJSON_IsTrue(cJSON_GetObjectItem(json, "ok")));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(json, "agent_models")) == 0);
    cJSON_Delete(json); cJSON_Delete(request);
}
