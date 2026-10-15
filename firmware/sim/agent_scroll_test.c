#include "../main/agent_menu.h"
#include "../main/face_agent.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DT (1.f / 30)

static void page(agent_menu_reply_t *r, uint16_t rid, uint8_t cursor, unsigned total, unsigned count) {
    memset(r, 0, sizeof *r);
    r->rid = rid; r->cursor = cursor; r->total = (uint8_t)total; r->count = (uint8_t)count; r->has_catalog = true;
    for (unsigned i = 0; i < count; i++) {
        snprintf(r->models[i].id, sizeof r->models[i].id, "model-%u", cursor * AGENT_PAGE_SIZE + i);
        snprintf(r->models[i].label, sizeof r->models[i].label, "Model %u", cursor * AGENT_PAGE_SIZE + i);
    }
}
static bool load(agent_menu_t *m, uint8_t cursor, unsigned total, unsigned count) {
    agent_menu_reply_t r;
    uint16_t rid = agent_menu_request_options(m, cursor, 100);
    assert(rid);
    page(&r, rid, cursor, total, count);
    return agent_menu_accept_reply(m, &r, 110);
}
static void open_models(agent_menu_t *m) {
    agent_menu_reset(m); agent_menu_show(m, true);
    agent_menu_visit(m, AGENT_VIEW_MODELS, AGENT_TARGET_AGENT);
}

static void test_pages_join_into_one_list(void) {
    agent_menu_t m; open_models(&m);
    uint8_t cursor = 9;
    assert(!agent_menu_next_page(&m, &cursor));  // nothing loaded yet: the first page comes from the opener
    assert(load(&m, 0, 9, 4) && agent_menu_next_page(&m, &cursor) && cursor == 1);
    agent_menu_request_options(&m, cursor, 120);
    assert(!agent_menu_next_page(&m, &cursor));  // one request at a time
    m.request_pending = false; m.wire_rid = 0;
    assert(load(&m, 1, 9, 4) && load(&m, 2, 9, 1));
    assert(m.count == 9 && !strcmp(m.models[8].id, "model-8") && !strcmp(m.models[0].id, "model-0"));
    assert(!agent_menu_next_page(&m, &cursor));
    agent_menu_clear_catalog(&m);
}

static void test_cap_gap_and_short_page(void) {
    agent_menu_t m; open_models(&m);
    assert(load(&m, 0, 128, 4) && agent_menu_listed(&m) == AGENT_LIST_MAX);
    assert(load(&m, 2, 128, 1) && m.count == 0 && !m.options_loaded);  // a gap starts over
    assert(!load(&m, 8, 128, 1));                                      // beyond the list cap
    agent_menu_clear_catalog(&m); open_models(&m);
    assert(load(&m, 0, 9, 4) && load(&m, 1, 9, 0) && m.total == 4);   // a short page ends the list
    uint8_t cursor;
    assert(!agent_menu_next_page(&m, &cursor));
    agent_menu_clear_catalog(&m);
}

static void drag_to(agent_menu_t *m, int from, int to) {
    agent_menu_drag(m, 200, from, true, false);
    int step = to < from ? -15 : 15;
    for (int y = from + step; step < 0 ? y >= to : y <= to; y += step) {
        agent_menu_drag(m, 200, y, false, false); agent_menu_tick(m, DT);
    }
}

static void test_drag_follows_finger(void) {
    agent_menu_t m; open_models(&m);
    m.options_loaded = true; m.count = m.total = 20;
    float max = agent_menu_scroll_max(&m);
    assert(max == 20 * AGENT_ROW_PITCH - (AGENT_LIST_BOTTOM - AGENT_LIST_TOP) + 20);
    agent_menu_drag(&m, 127, 164, true, false);  // a tap with a little wobble selects; the list stays
    assert(agent_menu_drag(&m, 132, 169, false, false) && m.scroll == 0 && agent_menu_take_tap(&m));
    drag_to(&m, 300, 180);                        // follows the finger without a jump at the dead zone
    assert(m.scrolling && m.scroll == 105 && !agent_menu_take_tap(&m));  // 285 - 180
    assert(face_agent_hit(&m, 40, 117 + 2 * 78 - 105) == AGENT_HIT_MODEL_0 + 2);
    assert(face_agent_hit(&m, 40, 117 + 78 - 105) == AGENT_HIT_NONE);    // faded under the header
    agent_menu_hide(&m);
    assert(!agent_menu_drag(&m, 127, 164, true, false));
}

static void test_scrolling_never_selects(void) {
    agent_menu_t m; open_models(&m);
    m.options_loaded = true; m.count = m.total = 20;
    for (int dx = -30; dx <= 30; dx += 10) for (int dy = 8; dy <= 60; dy += 4) {  // any drag, slow or short, diagonal too
        agent_menu_drag(&m, 200, 300, true, false);
        agent_menu_drag(&m, 200 + dx, 300 - dy, false, false);
        agent_menu_drag(&m, 200 + dx, 300 - dy, false, true);
        assert(!agent_menu_take_tap(&m) || (abs(dx) < 8 && dy < 8));
        m.scroll = m.scroll_v = 0;
    }
    agent_menu_drag(&m, 200, 300, true, false);   // a steady finger is still a tap
    agent_menu_drag(&m, 203, 302, false, false);
    assert(agent_menu_take_tap(&m));
    drag_to(&m, 300, 180);                         // touching a coasting list stops it without selecting
    agent_menu_drag(&m, 200, 180, false, true);
    assert(m.scroll_v > 0);
    agent_menu_drag(&m, 200, 250, true, false);
    assert(m.scroll_v == 0 && !agent_menu_take_tap(&m));
    agent_menu_drag(&m, 200, 250, true, false);    // the next steady touch taps again
    assert(agent_menu_take_tap(&m));
}

static void test_swipe_up_scrolls_instead_of_closing(void) {
    agent_menu_t m; open_models(&m);
    assert(agent_menu_owns_swipe(&m));
    agent_menu_visit(&m, AGENT_VIEW_OVERVIEW, AGENT_TARGET_AGENT);
    assert(!agent_menu_owns_swipe(&m));
    agent_menu_hide(&m);
    assert(!agent_menu_owns_swipe(&m));
}

static void test_release_coasts_and_stops(void) {
    agent_menu_t m; open_models(&m);
    m.options_loaded = true; m.count = m.total = 20;
    float max = agent_menu_scroll_max(&m);
    drag_to(&m, 300, 180);
    agent_menu_drag(&m, 200, 180, false, true);
    assert(!m.scrolling && m.scroll_v > 300);
    float at_release = m.scroll, speed = m.scroll_v;
    for (int i = 0; i < 20; i++) { agent_menu_tick(&m, DT); assert(m.scroll_v <= speed); speed = m.scroll_v; }
    assert(m.scroll > at_release + 40 && m.scroll <= max);
    for (int i = 0; i < 150; i++) agent_menu_tick(&m, DT);
    assert(m.scroll_v == 0 && m.scroll <= max);
}

static void test_rest_and_edges(void) {
    agent_menu_t m; open_models(&m);
    m.options_loaded = true; m.count = m.total = 20;
    drag_to(&m, 300, 250);                       // a finger resting before release does not throw the list
    for (int i = 0; i < 10; i++) agent_menu_tick(&m, DT);
    agent_menu_drag(&m, 200, 250, false, true);
    float resting = m.scroll;
    for (int i = 0; i < 10; i++) agent_menu_tick(&m, DT);
    assert(m.scroll - resting < 6);
    drag_to(&m, 100, 460);                       // the first row holds at the top
    assert(m.scroll < 0 && m.scroll >= -AGENT_OVERSCROLL);  // stretches while the finger pulls
    agent_menu_drag(&m, 200, 460, false, true);
    for (int i = 0; i < 90; i++) agent_menu_tick(&m, DT);
    assert(m.scroll == 0 && m.scroll_v == 0);               // and springs back
    drag_to(&m, 460, 20);                        // and the last at the bottom
    assert(m.scroll > 0 && m.scroll <= agent_menu_scroll_max(&m) + AGENT_OVERSCROLL);
    agent_menu_drag(&m, 200, 20, false, true);
    for (int i = 0; i < 150; i++) agent_menu_tick(&m, DT);
    assert(m.scroll_v == 0 && m.scroll <= agent_menu_scroll_max(&m));
    // A hard flick into the end stretches the list a little, then it eases back onto the last row.
    m.scroll = agent_menu_scroll_max(&m) - 30; m.scroll_v = 2000;
    float peak = 0;
    for (int i = 0; i < 150; i++) { agent_menu_tick(&m, DT); if (m.scroll > peak) peak = m.scroll; }
    assert(peak > agent_menu_scroll_max(&m) && peak <= agent_menu_scroll_max(&m) + AGENT_OVERSCROLL);
    assert(m.scroll == agent_menu_scroll_max(&m) && m.scroll_v == 0);
}

// A full-screen swipe down from a slightly scrolled list hits the top at speed: it must glide into the
// stretch and ease back, never jump (the largest per-frame step past the end stays small).
static void test_hard_swipe_to_top_is_smooth(void) {
    agent_menu_t m; open_models(&m);
    m.options_loaded = true; m.count = m.total = 20;
    m.scroll = 60;
    agent_menu_drag(&m, 240, 40, true, false);
    agent_menu_tick(&m, DT);
    for (int i = 1; i <= 6; i++) { agent_menu_drag(&m, 240, 40 + i * 70, false, false); agent_menu_tick(&m, DT / 2); }
    agent_menu_drag(&m, 240, 460, false, true);
    float prev = m.scroll, worst = 0;
    for (int i = 0; i < 90; i++) {
        agent_menu_tick(&m, DT);
        float step = fabsf(m.scroll - prev);
        if (m.scroll < 0 && step > worst) worst = step;
        prev = m.scroll;
        assert(m.scroll >= -AGENT_OVERSCROLL);
    }
    assert(worst < 28);
    assert(m.scroll == 0 && m.scroll_v == 0);
}

// The panel reports a lift about three samples late: a flick must still glide after those idle frames.
static void test_late_lift_still_glides(void) {
    agent_menu_t m; open_models(&m);
    m.options_loaded = true; m.count = m.total = 20;
    m.scroll = 200;
    agent_menu_drag(&m, 240, 420, true, false);
    for (int i = 1; i <= 8; i++) { agent_menu_drag(&m, 240, 420 - i * 30, false, false); agent_menu_tick(&m, DT); }
    for (int i = 0; i < 3; i++) agent_menu_tick(&m, DT);  // finger already gone, lift not yet reported
    agent_menu_drag(&m, 240, 180, false, true);
    float lifted = m.scroll;
    for (int i = 0; i < 60; i++) agent_menu_tick(&m, DT);
    assert(m.scroll - lifted > 120);
}

int main(void) {
    test_pages_join_into_one_list();
    test_cap_gap_and_short_page();
    test_drag_follows_finger();
    test_scrolling_never_selects();
    test_swipe_up_scrolls_instead_of_closing();
    test_release_coasts_and_stops();
    test_rest_and_edges();
    test_hard_swipe_to_top_is_smooth();
    test_late_lift_still_glides();
    puts("ok: model pages join into one scrolling list; finger drag, inertia, edges and tap/scroll separation");
    return 0;
}
