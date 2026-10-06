#include "../main/agent_menu.h"
#include "../main/face_agent.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_model(agent_model_t *model, const char *id, const char *label) {
    snprintf(model->id, sizeof model->id, "%s", id);
    snprintf(model->label, sizeof model->label, "%s", label);
}

static void set_page(agent_menu_reply_t *reply, uint16_t rid, uint8_t cursor) {
    memset(reply, 0, sizeof(*reply));
    reply->rid = rid;
    reply->cursor = cursor;
    reply->total = 5;
    reply->count = cursor ? 1 : 4;
    reply->has_catalog = true;
    reply->has_model = true;
    reply->has_stt = reply->has_tts = true;
    reply->stt_available = true;
    reply->tts_available = false;
    snprintf(reply->model, sizeof reply->model, "model-one");
    snprintf(reply->stt_provider, sizeof reply->stt_provider, "OpenAI");
    snprintf(reply->stt_model, sizeof reply->stt_model, "whisper-1");
    snprintf(reply->tts_provider, sizeof reply->tts_provider, "Local");
    snprintf(reply->tts_model, sizeof reply->tts_model, "voice-default");
    static const char *const ids[] = {"model-one", "model-two", "model-three", "model-four"};
    static const char *const labels[] = {"Model One", "Model Two", "Model Three", "Model Four"};
    for (unsigned i = 0; i < reply->count; i++) set_model(&reply->models[i], ids[cursor ? 0 : i], labels[cursor ? 0 : i]);
}

static uint16_t test_options(agent_menu_t *menu, agent_menu_reply_t *reply) {
    assert(agent_menu_request_options(menu, 0, 0) == 0);
    agent_menu_show(menu, true);
    uint16_t rid = agent_menu_request_options(menu, 0, 100);
    assert(rid && menu->request_pending && menu->cursor == 0);
    set_page(reply, rid, 0);
    reply->models[0].label[0] = '\n';
    assert(!agent_menu_accept_reply(menu, reply, 120));
    set_page(reply, (uint16_t)(rid + 1), 0);
    assert(!agent_menu_accept_reply(menu, reply, 130));
    set_page(reply, rid, 1);
    assert(!agent_menu_accept_reply(menu, reply, 140));
    set_page(reply, rid, 0);
    assert(agent_menu_accept_reply(menu, reply, 150));
    assert(menu->options_loaded && menu->capabilities_known && menu->stt_available && !menu->tts_available);
    assert(menu->count == 4 && menu->total == 5 && agent_menu_pages(menu) == 2);
    assert(agent_menu_current(menu, 0) && !agent_menu_current(menu, 1));
    return rid;
}

static void test_selection_ack(agent_menu_t *menu, agent_menu_reply_t *reply) {
    uint16_t rid = agent_menu_request_model(menu, "model-two", 200);
    assert(rid && agent_menu_current(menu, 0) && !agent_menu_current(menu, 1));
    set_page(reply, rid, 0);
    snprintf(reply->error, sizeof reply->error, "busy");
    assert(agent_menu_accept_reply(menu, reply, 250));
    assert(!menu->request_pending && !strcmp(menu->model, "model-one") && menu->retry_kind == AGENT_REQUEST_SELECT);
    rid = agent_menu_retry(menu, 300);
    assert(rid && menu->request_pending && menu->waiting_kind == AGENT_REQUEST_SELECT);
    set_page(reply, rid, 0);
    snprintf(reply->model, sizeof reply->model, "model-two");
    assert(agent_menu_accept_reply(menu, reply, 350));
    assert(agent_menu_current(menu, 1) && !agent_menu_current(menu, 0));
    agent_menu_set_online(menu, false);
    assert(!menu->capabilities_known && menu->retry_kind == AGENT_REQUEST_NONE);
    agent_menu_set_online(menu, true);
    assert(!menu->capabilities_known);
}

static void test_requests_and_ack(void) {
    agent_menu_t menu;
    agent_menu_reply_t reply;
    agent_menu_reset(&menu);
    test_options(&menu, &reply);
    test_selection_ack(&menu, &reply);
}

static void test_timeout_and_bounds(void) {
    agent_menu_t menu;
    agent_menu_reset(&menu);
    agent_menu_show(&menu, true);
    uint16_t rid = agent_menu_request_options(&menu, 1, 1000);
    assert(rid && !agent_menu_timeout(&menu, 8999));
    assert(agent_menu_timeout(&menu, 9000) && !menu.request_pending && !strcmp(menu.error, "timeout"));
    assert(agent_menu_retry(&menu, 9001) && menu.waiting_cursor == 1);
    agent_menu_set_online(&menu, false);
    assert(!menu.online && !menu.request_pending);
    assert(!agent_menu_id_valid("") && !agent_menu_id_valid("bad\nmodel"));
    char too_long[AGENT_MODEL_ID_MAX + 2];
    memset(too_long, 'x', sizeof too_long);
    too_long[sizeof too_long - 1] = 0;
    assert(!agent_menu_id_valid(too_long));
    assert(agent_menu_error_valid("denied") && !agent_menu_error_valid("raw server text"));
}

static void test_selection_on_later_page(void) {
    agent_menu_t menu;
    agent_menu_reply_t reply;
    agent_menu_reset(&menu);
    agent_menu_show(&menu, true);
    uint16_t rid = agent_menu_request_options(&menu, 2, 100);
    set_page(&reply, rid, 2);
    reply.total = 9;
    assert(agent_menu_accept_reply(&menu, &reply, 110));
    rid = agent_menu_request_model(&menu, "model-one", 120);
    assert(menu.waiting_cursor == 2);
    set_page(&reply, rid, 0);
    assert(!agent_menu_accept_reply(&menu, &reply, 130));
    set_page(&reply, rid, 2);
    reply.total = 9;
    assert(agent_menu_accept_reply(&menu, &reply, 140));
    rid = agent_menu_request_model(&menu, "model-one", 150);
    reply = (agent_menu_reply_t){.rid = rid, .cursor = 2};
    strcpy(reply.error, "busy");
    assert(agent_menu_accept_reply(&menu, &reply, 160));
    assert(!strcmp(menu.error, "busy") && menu.retry_cursor == 2);
}

static unsigned pushed_pixels;
static void count_push(int x0, int y0, int x1, int y1, uint16_t *pixels, void *ctx) {
    (void)ctx;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) pushed_pixels += pixels[(y - y0) * (x1 - x0) + x - x0] != 0;
}

static void test_ui_and_taps(void) {
    agent_menu_t menu;
    agent_menu_reply_t reply;
    agent_menu_reset(&menu);
    agent_menu_show(&menu, true);
    uint16_t rid = agent_menu_request_options(&menu, 0, 1);
    set_page(&reply, rid, 0);
    assert(agent_menu_accept_reply(&menu, &reply, 2));
    assert(face_agent_hit(&menu, 88, 52) == AGENT_HIT_NONE);
    assert(face_agent_hit(&menu, 240, 196) == AGENT_HIT_MODELS);
    assert(face_agent_hit(&menu, 127, 306) == AGENT_HIT_VOICE);
    assert(face_agent_hit(&menu, 353, 410) == AGENT_HIT_VOICE);
    assert(face_agent_hit(&menu, 240, 254) == AGENT_HIT_NONE);
    menu.view = AGENT_VIEW_MODELS;
    assert(face_agent_hit(&menu, 40, 101) == AGENT_HIT_MODEL_0);
    assert(face_agent_hit(&menu, 440, 227) == AGENT_HIT_MODEL_0);
    assert(face_agent_hit(&menu, 40, 241) == AGENT_HIT_MODEL_1);
    assert(face_agent_hit(&menu, 440, 367) == AGENT_HIT_MODEL_1);
    assert(face_agent_hit(&menu, 40, 234) == AGENT_HIT_NONE);
    assert(face_agent_hit(&menu, 392, 428) == AGENT_HIT_NEXT);
    assert(face_agent_hit(&menu, 88, 428) == AGENT_HIT_NONE);
    assert(face_agent_hit(&menu, 19, 164) == AGENT_HIT_NONE);
    menu.half = 1;
    assert(face_agent_hit(&menu, 40, 164) == AGENT_HIT_MODEL_2);
    assert(face_agent_hit(&menu, 40, 304) == AGENT_HIT_MODEL_3);
    menu.request_pending = true;
    assert(face_agent_hit(&menu, 40, 164) == AGENT_HIT_NONE);
    assert(face_agent_hit(&menu, 392, 428) == AGENT_HIT_NONE);
    menu.request_pending = false;
    strcpy(menu.error, "timeout");
    menu.retry_kind = AGENT_REQUEST_OPTIONS;
    assert(face_agent_hit(&menu, 240, 428) == AGENT_HIT_RETRY);
    menu.target = AGENT_TARGET_STT; menu.error[0] = 0; menu.count = menu.total = 0;
    // Empty voice catalogs remain refreshable without an extra details page.
    assert(face_agent_hit(&menu, 240, 428) == AGENT_HIT_REFRESH);
    menu.view = AGENT_VIEW_MODELS;
    menu.online = false;
    assert(face_agent_hit(&menu, 40, 164) == AGENT_HIT_NONE);
    menu.online = true;
    menu.view = AGENT_VIEW_OVERVIEW;

    scene_t scene;
    scene_begin(&scene, 0);
    face_agent_draw(&scene, &menu, false);
    assert(scene.n < R_MAX_PRIMS && scene.text_n > 0);
    bool model_icon = false;
    for (int i = 0; i < scene.n; i++) model_icon |= scene.p[i].kind == PK_ICON && scene.p[i].b == ICON_BLOCKS;
    assert(model_icon);
    uint16_t canvas[R_W * R_H] = {0}, spare[R_W * R_H] = {0};
    uint16_t *buffers[] = {canvas, spare};
    render_state_t state = {0};
    pushed_pixels = 0;
    render_frame(&state, &scene, buffers, 2, false, count_push, NULL);
    assert(pushed_pixels > 1000);
    uint16_t overlay[R_W * R_H * 4] = {0};
    render_text_overlay(&scene, overlay, 0, 0, R_W * 2, R_H * 2, false);
    unsigned visible = 0;
    for (unsigned i = 0; i < sizeof overlay / sizeof overlay[0]; i++) visible += overlay[i] != 0;
    assert(visible > 1000);
}

static void test_large_pages_and_drag(void) {
    agent_menu_t menu;
    agent_menu_reply_t reply;
    agent_menu_reset(&menu);
    test_options(&menu, &reply);
    menu.view = AGENT_VIEW_MODELS;
    uint8_t cursor = 0;
    assert(!agent_menu_turn_page(&menu, true, &cursor) && menu.half == 1);
    assert(agent_menu_display_page(&menu) == 1 && agent_menu_display_pages(&menu) == 3);
    assert(agent_menu_turn_page(&menu, true, &cursor) && cursor == 1 && menu.half == 0);
    uint16_t rid = agent_menu_request_options(&menu, cursor, 200);
    assert(!agent_menu_can_turn(&menu, false));
    set_page(&reply, rid, cursor);
    assert(agent_menu_accept_reply(&menu, &reply, 210));
    assert(!agent_menu_can_turn(&menu, true));
    assert(agent_menu_turn_page(&menu, false, &cursor) && cursor == 0 && menu.half == 1);
    rid = agent_menu_request_options(&menu, cursor, 220);
    set_page(&reply, rid, cursor);
    assert(agent_menu_accept_reply(&menu, &reply, 230) && menu.half == 1);
    assert(!agent_menu_turn_page(&menu, false, &cursor) && menu.half == 0);
    assert(agent_menu_drag(&menu, 127, 164, true));
    assert(agent_menu_drag(&menu, 132, 169, false) && agent_menu_take_tap(&menu));
    agent_menu_drag(&menu, 127, 164, true);
    agent_menu_drag(&menu, 127, 194, false);
    assert(!agent_menu_take_tap(&menu));
    agent_menu_hide(&menu);
    assert(!agent_menu_drag(&menu, 127, 164, true));
}

static void test_view_bounds(void) {
    agent_menu_t menu;
    agent_menu_reply_t reply;
    agent_menu_reset(&menu);
    test_options(&menu, &reply);
    for (int view = AGENT_VIEW_OVERVIEW; view <= AGENT_VIEW_CLASSIC; view++) {
        menu.view = (agent_view_t)view;
        menu.guide_elapsed = 1;
        scene_t scene;
        scene_begin(&scene, 0);
        face_agent_draw(&scene, &menu, false);
        assert(scene.n < R_MAX_PRIMS);
        // English fixture data must produce English UI copy on every native page.
        for (int i = 0; i < scene.text_n; i++) assert((unsigned char)scene.text[i] < 128);
        int x0, y0, x1, y1;
        render_text_signature(&scene, &x0, &y0, &x1, &y1);
        // Bounds use the 240px canvas. The 40px heading starts at native y14;
        // include two bilinear-neighbour pixels (minimum y5).
        assert(x0 >= 10 && x1 <= 232 && y0 >= 5 && y1 <= 232);
    }
}

static void check_refresh_bounds(agent_menu_t *m) {
    for (unsigned frame = 0; frame < 7; frame++) {
        m->guide_elapsed = frame / 30.f;
        scene_t scene; scene_begin(&scene, 0); face_agent_draw(&scene, m, false);
        assert(face_agent_hit(m, 240, 328) == AGENT_HIT_REFRESH);
        assert(face_agent_hit(m, 240, 384) == AGENT_HIT_REFRESH);
        assert(face_agent_hit(m, 240, 327) == AGENT_HIT_NONE);
        assert(face_agent_hit(m, 240, 385) == AGENT_HIT_NONE);
        bool found = false;
        for (int i = 0; i < scene.n; i++) {
            const prim_t *p = &scene.p[i];
            if (p->kind == PK_RBOX && p->cy == 356 * 16) found = true;
        }
        assert(found); // The refresh tile fades but never moves away from its touch rectangle.
    }
}

static void text_ink_height(const scene_t *s, const prim_t *p, int *top, int *bottom) {
    *top = 480; *bottom = 0;
    for (int i = 0; i < p->b; i++) {
        const font_glyph_t *g = &g_fonts[p->r].g[s->text[p->a + i]];
        if (!g->h) continue;
        int y = p->sub_y - g->top;
        if (y < *top) *top = y;
        if (y + g->h > *bottom) *bottom = y + g->h;
    }
}
static void check_guide_spacing(const scene_t *scene) {
    for (int i = 0; i < scene->n; i++) {
        const prim_t *p = &scene->p[i];
        if (p->kind != PK_TEXT || !p->alpha) continue;
        assert(p->r != FONT_BIG); // No duplicate oversized Guide/page headers.
        // The central hardware cue shares the header with Skip, not with its hit area.
        if (p->by1 < 80) assert(p->bx0 >= 356 || (p->bx0 >= 190 && p->bx1 <= 290));
        int py0, py1; text_ink_height(scene, p, &py0, &py1);
        for (int j = i + 1; j < scene->n; j++) {
            const prim_t *q = &scene->p[j];
            if (q->kind != PK_TEXT || !q->alpha) continue;
            int qy0, qy1; text_ink_height(scene, q, &qy0, &qy1);
            assert(py1 + 8 <= qy0 || qy1 + 8 <= py0 ||
                   p->bx1 + 8 <= q->bx0 || q->bx1 + 8 <= p->bx0);
        }
    }
}

static void check_guide_cue(const scene_t *scene, const agent_menu_t *m);
static void test_guide(void) {
    agent_menu_t m; agent_menu_reset(&m); agent_menu_start_guide(&m, true);
    assert(m.open && m.view == AGENT_VIEW_GUIDE && m.guide_step == 0);
    assert(face_agent_hit(&m, 392, 52) == AGENT_HIT_GUIDE_SKIP);
    for (unsigned known = 0; known < 2; known++) for (unsigned caps = 0; caps < 4; caps++) {
        m.capabilities_known = known; m.stt_available = caps & 1; m.tts_available = caps & 2;
        for (unsigned step = 0; step < 4; step++) for (unsigned frame = 0; frame < 7; frame++) {
            m.guide_step = step; m.guide_elapsed = frame / 30.f; m.volume = frame % 2 ? 19 : 20;
            scene_t scene; scene_begin(&scene, 0); face_agent_draw(&scene, &m, frame % 2 == 0);
            assert(scene.n < R_MAX_PRIMS);
            check_guide_spacing(&scene);
            check_guide_cue(&scene, &m);
            int x0, y0, x1, y1; render_text_signature(&scene, &x0, &y0, &x1, &y1);
            assert(x0 >= 10 && x1 <= 232 && y0 >= 5 && y1 <= 232);
            assert(face_agent_hit(&m, 240, 428) == (step == 3 ? AGENT_HIT_GUIDE_DONE : AGENT_HIT_GUIDE_NEXT));
        }
    }
    m.guide_step = 0; agent_menu_guide_move(&m, -1); assert(m.guide_step == 0);
    agent_menu_guide_move(&m, 1); assert(m.guide_step == 1 && m.guide_elapsed == 0);
    m.capabilities_known = true; m.stt_available = false;
    check_refresh_bounds(&m);
    assert(face_agent_hit(&m, 240, 360) == AGENT_HIT_REFRESH);
    m.request_pending = true; assert(face_agent_hit(&m, 240, 360) == AGENT_HIT_NONE);
    m.request_pending = false; m.online = false; assert(face_agent_hit(&m, 240, 360) == AGENT_HIT_NONE);
    m.guide_step = 3; agent_menu_guide_move(&m, 1); assert(m.guide_step == 3);
    agent_menu_start_guide(&m, true); assert(m.guide_step == 0);
    assert(!agent_menu_request_model(&m, "unlisted", 0));
}

static bool scene_has_label(const scene_t *s, const char *text) {
    for (int i = 0; i < s->n; i++) {
        const prim_t *p = &s->p[i];
        if (p->kind != PK_TEXT) continue;
        const char *at = text;
        int j = 0;
        while (*at && j < p->b && s->text[p->a + j] == font_glyph(&g_fonts[p->r], font_utf8_next(&at))) j++;
        if (!*at && j == p->b) return true;
    }
    return false;
}
static int cue_stroke(const prim_t *p, bool boot) {
    float x = p->cx / 16.f, y = p->cy / 16.f;
    if (p->kind != PK_RBOX || y >= 64 || x <= 120 || x >= 350) return -1;
    // Shape bounds are populated during rasterization; use the actual scene geometry here.
    float dx = (abs(p->cs) * (float)p->a + abs(p->sn) * (float)p->b) / (16384.f * 16);
    float dy = (abs(p->sn) * (float)p->a + abs(p->cs) * (float)p->b) / (16384.f * 16);
    assert(y - dy >= 0 && y + dy < 64 && x - dx > 120 && x + dx < 350);
    assert(boot ? x + dx < 210 : x - dx > 270);
    return y - dy < 12; // Whether this stroke reaches the physical-button edge.
}
static void check_guide_cue(const scene_t *scene, const agent_menu_t *m) {
    bool boot = m->guide_step == 0 || m->guide_step == 3;
    bool key = m->guide_step == 1 && m->online && m->capabilities_known && m->stt_available;
    assert(scene_has_label(scene, "BOOT") == boot);
    assert(scene_has_label(scene, "KEY") == key);
    assert(!scene_has_label(scene, "PWR")); // No power instruction on these four pages.
    unsigned strokes = 0, tips = 0;
    for (int i = 0; i < scene->n; i++) {
        // Real pointer geometry between progress and Skip. Neither gets covered.
        int reaches_edge = cue_stroke(&scene->p[i], boot);
        if (reaches_edge < 0) continue;
        strokes++;
        tips += reaches_edge;
    }
    assert(strokes == ((boot || key) ? 4u : 0u));
    assert(tips == ((boot || key) ? 3u : 0u));
    assert(face_agent_hit(m, 240, 42) == AGENT_HIT_NONE); // Cue is not a screen button.
}
static void test_voice_instructions(void) {
    agent_menu_t m; agent_menu_reset(&m); agent_menu_start_guide(&m, true);
    m.guide_step = 1; m.guide_elapsed = 1;
    agent_menu_set_capabilities(&m, true, true);
    for (unsigned tess = 0; tess < 2; tess++) for (int mode = VOICE_CLASSIC; mode <= VOICE_LIVE; mode++) {
        m.voice_mode = mode;
        scene_t scene; scene_begin(&scene, 0); face_agent_draw(&scene, &m, tess);
        bool wake = tess && mode != VOICE_LIVE;
        assert(scene_has_label(&scene, "Home: 'Hi Tessa', then talk.") == wake);
        assert(scene_has_label(&scene, "Hold KEY to talk.") == (!tess && mode == VOICE_CLASSIC));
        assert(scene_has_label(&scene, "Press KEY, then talk.") == (!tess && mode != VOICE_CLASSIC));
        assert(scene_has_label(&scene, "Say 'Hi Tessa' or press KEY.") == (tess && mode == VOICE_LIVE));
        assert(scene_has_label(&scene, "Press KEY to end GPT Live.") == (mode == VOICE_LIVE));
        assert(scene_has_label(&scene, "Pause to send.") == (wake || mode == VOICE_REALTIME));
        assert(scene_has_label(&scene, "Or hold KEY; release to send.") == (tess && mode == VOICE_CLASSIC));
        check_guide_spacing(&scene);
        check_guide_cue(&scene, &m);
    }
    m.online = false;
    scene_t scene; scene_begin(&scene, 0); face_agent_draw(&scene, &m, true);
    check_guide_cue(&scene, &m);
}

static void test_voice_catalog_scope(void) {
    agent_menu_t menu; agent_menu_reply_t reply;
    agent_menu_reset(&menu); test_options(&menu, &reply);
    menu.target = AGENT_TARGET_STT; menu.view = AGENT_VIEW_MODELS;
    uint16_t rid = agent_menu_request_options(&menu, 0, 200);
    set_page(&reply, rid, 0);
    assert(!agent_menu_accept_reply(&menu, &reply, 220)); // delayed Agent data cannot paint Input
    reply.target = AGENT_TARGET_STT;
    strcpy(reply.model, "voice-two"); set_model(&reply.models[1], "voice-two", "Voice Two");
    assert(agent_menu_accept_reply(&menu, &reply, 230));
    assert(!strcmp(menu.model, "model-one") && !strcmp(menu.selected_model, "voice-two"));
    assert(agent_menu_current(&menu, 1) && !agent_menu_current(&menu, 0));
    assert(face_agent_hit(&menu, 240, 304) == AGENT_HIT_MODEL_1);
    menu.request_pending = true;
    assert(face_agent_hit(&menu, 240, 354) == AGENT_HIT_NONE);
    menu.request_pending = false; menu.count = 0;
    assert(face_agent_hit(&menu, 240, 354) == AGENT_HIT_NONE);
    menu.target = AGENT_TARGET_AGENT;
    rid = agent_menu_request_options(&menu, 0, 240);
    reply.rid = rid;
    assert(!agent_menu_accept_reply(&menu, &reply, 250)); // delayed Input cannot replace Agent catalog

}

static void test_voice_modes_and_parents(void) {
    agent_menu_t m; agent_menu_reset(&m); agent_menu_show(&m, true);
    agent_menu_visit(&m, AGENT_VIEW_VOICE_MODES, AGENT_TARGET_MODE);
    uint16_t rid = agent_menu_request_options(&m, 0, 10);
    agent_menu_reply_t r = {.target=AGENT_TARGET_MODE, .rid=rid, .has_catalog=true, .has_model=true, .total=3, .count=3};
    strcpy(r.model, "classic");
    set_model(&r.models[0], "classic", "STT"); set_model(&r.models[1], "realtime", "Realtime");
    set_model(&r.models[2], "live", "GPT Live"); r.models[2].disabled = true;
    assert(agent_menu_accept_reply(&m, &r, 20) && m.voice_mode == VOICE_CLASSIC);
    assert(face_agent_hit(&m, 44, 100) == AGENT_HIT_MODEL_0);
    assert(face_agent_hit(&m, 440, 280) == AGENT_HIT_MODEL_1);
    assert(face_agent_hit(&m, 240, 350) == AGENT_HIT_NONE);
    assert(!agent_menu_request_model(&m, "live", 30));
    rid = agent_menu_request_model(&m, "realtime", 30); assert(rid);
    assert(m.voice_mode == VOICE_CLASSIC); // No optimistic switch.
    assert(face_agent_hit(&m, 240, 238) == AGENT_HIT_NONE);
    r.rid = rid; strcpy(r.error, "unavailable");
    assert(agent_menu_accept_reply(&m, &r, 40) && m.voice_mode == VOICE_CLASSIC);
    rid = agent_menu_request_model(&m, "realtime", 50);
    r.rid = rid; r.error[0] = 0; strcpy(r.model, "realtime");
    assert(agent_menu_accept_reply(&m, &r, 60) && m.voice_mode == VOICE_REALTIME);
    agent_menu_voice_models(&m); assert(m.target == AGENT_TARGET_VOICE && m.view == AGENT_VIEW_MODELS);
    assert(!agent_menu_accept_reply(&m, &r, 70)); // Delayed mode response cannot repaint models.
    agent_menu_back(&m); assert(m.view == AGENT_VIEW_VOICE_MODES);
    m.voice_mode = VOICE_CLASSIC; agent_menu_voice_models(&m); assert(m.view == AGENT_VIEW_CLASSIC);
    assert(face_agent_hit(&m, 240, 200) == AGENT_HIT_STT);
    assert(face_agent_hit(&m, 240, 360) == AGENT_HIT_TTS);
    agent_menu_visit(&m, AGENT_VIEW_MODELS, AGENT_TARGET_STT); agent_menu_back(&m);
    assert(m.view == AGENT_VIEW_CLASSIC);
    agent_menu_back(&m); assert(m.view == AGENT_VIEW_VOICE_MODES);
    agent_menu_back(&m); assert(m.view == AGENT_VIEW_OVERVIEW);
    agent_menu_back(&m); assert(!m.open);
}

static void test_back_during_save(void) {
    agent_menu_t m; agent_menu_reply_t r; agent_menu_reset(&m); test_options(&m, &r);
    uint16_t rid = agent_menu_request_model(&m, "model-two", 200);
    agent_menu_visit(&m, AGENT_VIEW_VOICE_MODES, AGENT_TARGET_MODE);
    assert(!agent_menu_request_options(&m, 0, 210)); // Host is still saving; no Busy-producing request.
    set_page(&r, rid, 0); strcpy(r.model, "model-two");
    assert(!agent_menu_accept_reply(&m, &r, 220)); // Old page cannot paint Voice.
    assert(!m.error[0] && !m.wire_rid && !m.options_loaded);
    assert(agent_menu_request_options(&m, 0, 230));
    agent_menu_back(&m);
    assert(!agent_menu_request_options(&m, 0, 240));
    agent_menu_timeout(&m, 8230); // Lost response still has a bounded wait.
    assert(agent_menu_request_options(&m, 0, 8231));
    agent_menu_set_online(&m, false); assert(!m.wire_rid);
}

int main(void) {
    test_back_during_save();
    test_voice_modes_and_parents();
    test_voice_catalog_scope();
    test_voice_instructions();
    test_requests_and_ack();
    test_timeout_and_bounds();
    test_selection_on_later_page();
    test_ui_and_taps();
    test_large_pages_and_drag();
    test_view_bounds();
    test_guide();
    puts("ok: Agent screen renders within primitive budget; hit bounds, pages, stale ACKs, retry, and capability gate state");
    return 0;
}
