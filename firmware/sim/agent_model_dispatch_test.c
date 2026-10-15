#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "agent_menu.h"
static struct { agent_menu_t agent; } g_face;
static unsigned sent_cursor, sent_rid, wakes;
static void face_lock(void) {} static void face_unlock(void) {}
static unsigned now_ms(void) { return 100; }
static void disp_wake(void) { wakes++; }
static bool send_agent_request(const char *kind, uint16_t rid, uint8_t cursor, const char *id) {
    assert(!strcmp(kind, "agent_model") && !strcmp(id, "model-0"));
    sent_cursor = cursor; sent_rid = rid; return true;
}
/* PRODUCTION_REQUEST */
int main(void) {
    agent_menu_visit(&g_face.agent, AGENT_VIEW_MODELS, AGENT_TARGET_AGENT);
    assert(agent_menu_reserve_catalog(&g_face.agent));
    g_face.agent.online = true; g_face.agent.cursor = 2;
    g_face.agent.count = 1;
    strcpy(g_face.agent.models[0].id, "model-0");
    request_agent_model("model-0");
    assert(sent_rid && sent_cursor == 0 && wakes == 1);
    assert(g_face.agent.waiting_cursor == sent_cursor);
    agent_menu_clear_catalog(&g_face.agent);
}
