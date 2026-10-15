#include <assert.h>
#include <stdlib.h>
#include <string.h>
static int fail_alloc;
static void *catalog_calloc(size_t count, size_t size) {
    return fail_alloc ? NULL : calloc(count, size);
}
#define calloc catalog_calloc
#include "../main/agent_menu.c"
#undef calloc

static agent_menu_reply_t page(agent_menu_t *menu, unsigned cursor) {
    agent_menu_reply_t reply = {.has_catalog=true, .count=AGENT_PAGE_SIZE,
        .total=AGENT_LIST_MAX, .cursor=cursor, .target=AGENT_TARGET_AGENT};
    reply.rid = agent_menu_request_options(menu, cursor, 100 + cursor);
    assert(reply.rid);
    for (unsigned i=0; i<reply.count; ++i) {
        memset(reply.models[i].id, 'a' + cursor, AGENT_MODEL_ID_MAX);
        memset(reply.models[i].label, 'A' + cursor, AGENT_MODEL_LABEL_MAX);
        reply.models[i].id[0] = '0' + i;
    }
    return reply;
}
static void selected_reply_after_reclaim(void) {
    agent_menu_t m; agent_menu_reset(&m); agent_menu_show(&m,true);
    agent_menu_reply_t r=page(&m,0); assert(agent_menu_accept_reply(&m,&r,200));
    r.rid=agent_menu_request_model(&m,r.models[0].id,201);
    assert(r.rid); r.has_model=true; strcpy(r.model,r.models[0].id);
    agent_menu_hide(&m); agent_menu_reclaim_hidden(&m);
    assert(agent_menu_accept_reply(&m,&r,202));
    assert(!m.models && !m.count && !strcmp(m.model,r.model));
    agent_menu_show(&m,true); assert(!m.catalog_reclaimed);
    r=page(&m,0); assert(agent_menu_accept_reply(&m,&r,204) && m.models);
    agent_menu_clear_catalog(&m);
}
static void repeat_catalog(agent_menu_t *menu) {
    agent_menu_reply_t reply;
    for(unsigned i=0;i<1000;++i) {
        agent_menu_show(menu, true); reply=page(menu,0);
        assert(agent_menu_accept_reply(menu,&reply,300+i));
        agent_menu_visit(menu,AGENT_VIEW_OVERVIEW,AGENT_TARGET_AGENT);
        assert(!menu->models && !menu->count);
    }
}
int main(void) {
    selected_reply_after_reclaim();
    agent_menu_t menu; agent_menu_reset(&menu); agent_menu_show(&menu, true);
    assert(!menu.models);
    fail_alloc=1;
    agent_menu_reply_t reply=page(&menu, 0);
    assert(agent_menu_accept_reply(&menu, &reply, 200));
    assert(!menu.models && !menu.count && !menu.options_loaded);
    assert(!strcmp(menu.error,"unavailable") && menu.retry_kind==AGENT_REQUEST_OPTIONS);
    fail_alloc=0;
    reply=page(&menu, 0); assert(agent_menu_accept_reply(&menu, &reply, 201));
    for(unsigned cursor=1; cursor<AGENT_LIST_MAX/AGENT_PAGE_SIZE; ++cursor) {
        reply=page(&menu, cursor); assert(agent_menu_accept_reply(&menu, &reply, 202+cursor));
    }
    assert(menu.count==AGENT_LIST_MAX);
    for(unsigned i=0;i<AGENT_LIST_MAX;++i) {
        assert(strlen(menu.models[i].id)==AGENT_MODEL_ID_MAX);
        assert(strlen(menu.models[i].label)==AGENT_MODEL_LABEL_MAX);
    }
    agent_model_t *cache=menu.models;
    agent_menu_reclaim_hidden(&menu); assert(menu.models==cache); // open UI owns its options
    strcpy(menu.model,"selected"); strcpy(menu.selected_model,"selected");
    menu.voice_mode=VOICE_LIVE; menu.stt_available=menu.capabilities_known=true;
    agent_menu_hide(&menu); assert(menu.models==cache && menu.count==AGENT_LIST_MAX);
    agent_menu_reclaim_hidden(&menu);
    assert(!strcmp(menu.model,"selected") && !strcmp(menu.selected_model,"selected"));
    assert(menu.voice_mode==VOICE_LIVE && menu.stt_available && menu.capabilities_known);
    agent_menu_show(&menu,true); reply=page(&menu,0); agent_menu_hide(&menu);
    agent_menu_reclaim_hidden(&menu);
    assert(!agent_menu_accept_reply(&menu,&reply,203)); // abandoned options cannot reallocate during voice
    agent_menu_visit(&menu, AGENT_VIEW_OVERVIEW, AGENT_TARGET_AGENT);
    assert(!menu.models && !menu.count && !menu.total);
    repeat_catalog(&menu);
    agent_menu_clear_catalog(&menu); agent_menu_clear_catalog(&menu);
}
