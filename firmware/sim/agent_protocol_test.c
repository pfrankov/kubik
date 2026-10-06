#include "agent_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static cJSON *parse(const char *text) {
    cJSON *json = cJSON_Parse(text);
    assert(json);
    return json;
}

static void test_catalog(void) {
    const char *text = "{\"target\":\"agent\",\"rid\":41,\"cursor\":1,\"total\":5,\"model\":\"vendor/model\","
        "\"models\":[{\"id\":\"vendor/model\",\"label\":\"Vendor Model\"}],"
        "\"stt\":{\"available\":true,\"provider\":\"OpenAI\",\"model\":\"whisper\"},"
        "\"tts\":{\"available\":false,\"provider\":\"Local\"}}";
    cJSON *json = parse(text);
    agent_menu_reply_t reply;
    assert(agent_protocol_parse_options(json, &reply));
    assert(reply.rid == 41 && reply.cursor == 1 && reply.total == 5 && reply.count == 1);
    assert(reply.has_model && !strcmp(reply.model, "vendor/model"));
    cJSON_DeleteItemFromObject(json, "target");
    assert(!agent_protocol_parse_options(json, &reply));
    cJSON_AddStringToObject(json, "target", "voice");
    assert(agent_protocol_parse_options(json, &reply) && reply.target == AGENT_TARGET_VOICE);
    cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "models"), 0);
    cJSON_AddBoolToObject(row, "available", false);
    assert(agent_protocol_parse_options(json, &reply) && reply.models[0].disabled);
    cJSON_ReplaceItemInObject(row, "available", cJSON_CreateString("no"));
    assert(!agent_protocol_parse_options(json, &reply));
    cJSON_DeleteItemFromObject(row, "available");
    cJSON_ReplaceItemInObject(json, "target", cJSON_CreateString("stt"));
    assert(agent_protocol_parse_options(json, &reply) && reply.target == AGENT_TARGET_STT);
    assert(reply.has_stt && reply.stt_available && reply.has_tts && !reply.tts_available);
    assert(!strcmp(reply.models[0].label, "Vendor Model"));
    cJSON_Delete(json);
}

static void test_rejects_unsafe_values(void) {
    agent_menu_reply_t reply;
    cJSON *json = parse("{\"target\":\"agent\",\"rid\":2,\"cursor\":0,\"total\":1,\"model\":\"a\","
        "\"models\":[{\"id\":\"a\",\"label\":\"bad\\nlabel\"}],"
        "\"stt\":{\"available\":true},\"tts\":{\"available\":false}}");
    assert(!agent_protocol_parse_options(json, &reply));
    cJSON_Delete(json);
    json = parse("{\"target\":\"agent\",\"rid\":2.5,\"cursor\":0,\"total\":0,\"error\":\"busy\"}");
    assert(!agent_protocol_parse_options(json, &reply));
    cJSON_Delete(json);
    json = parse("{\"target\":\"agent\",\"rid\":2,\"cursor\":0,\"total\":0,\"error\":\"secret\"}");
    assert(!agent_protocol_parse_options(json, &reply));
    cJSON_Delete(json);
    json = parse("{\"target\":\"agent\",\"rid\":2,\"cursor\":0,\"total\":0,\"error\":\"busy\"}");
    assert(agent_protocol_parse_options(json, &reply) && !reply.has_catalog);
    cJSON_Delete(json);
}

static void test_capabilities_and_null_escape(void) {
    bool stt, tts;
    cJSON *json = parse("{\"stt\":{\"available\":true},\"tts\":{\"available\":false}}");
    assert(agent_protocol_parse_capabilities(json, &stt, &tts) && stt && !tts);
    cJSON_Delete(json);
    json = parse("{\"stt\":{\"available\":\"yes\"},\"tts\":{\"available\":false}}");
    assert(!agent_protocol_parse_capabilities(json, &stt, &tts));
    cJSON_Delete(json);
    const char *nul = "{\"id\":\"a\\u0000b\"}";
    const char *newline = "{\"id\":\"a\\n\"}";
    assert(agent_protocol_has_nul_escape(nul, strlen(nul)));
    assert(!agent_protocol_has_nul_escape(newline, strlen(newline)));
}

static void test_later_page_error(void) {
    agent_menu_t menu;
    agent_menu_reply_t reply;
    agent_menu_reset(&menu);
    agent_menu_show(&menu, true);
    uint16_t rid = agent_menu_request_options(&menu, 2, 100);
    cJSON *json = parse("{\"target\":\"agent\",\"rid\":1,\"cursor\":2,\"total\":0,\"error\":\"busy\"}");
    assert(rid == 1 && agent_protocol_parse_options(json, &reply));
    assert(agent_menu_accept_reply(&menu, &reply, 110));
    assert(!strcmp(menu.error, "busy") && !menu.request_pending);
    cJSON_Delete(json);
}

int main(void) {
    test_catalog();
    test_rejects_unsafe_values();
    test_capabilities_and_null_escape();
    test_later_page_error();
    puts("agent protocol: bounded catalogs, capabilities, safe strings and NUL escape rejection passed");
    return 0;
}
