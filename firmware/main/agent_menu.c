#include "agent_menu.h"

#include <string.h>

static bool copy_bounded(char *dst, size_t cap, const char *src) {
    if (!dst || !cap || !src) return false;
    size_t n = strnlen(src, cap);
    if (n >= cap) return false;
    memcpy(dst, src, n + 1);
    return true;
}

bool agent_menu_id_valid(const char *id) {
    if (!id) return false;
    size_t n = strnlen(id, AGENT_MODEL_ID_MAX + 1);
    if (!n || n > AGENT_MODEL_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)id[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool agent_menu_error_valid(const char *error) {
    static const char *const errors[] = {"busy", "timeout", "unsupported", "unavailable", "invalid_model", "denied"};
    if (!error || !error[0]) return true;
    for (unsigned i = 0; i < sizeof errors / sizeof errors[0]; i++)
        if (!strcmp(error, errors[i])) return true;
    return false;
}

const char *agent_target_name(agent_target_t target) {
    static const char *const names[] = {"agent", "stt", "tts", "mode", "voice"};
    return target <= AGENT_TARGET_VOICE ? names[target] : "agent";
}
bool agent_target_parse(const char *name, agent_target_t *target) {
    if (!name || !target) return false;
    for (unsigned i = 0; i <= AGENT_TARGET_VOICE; ++i)
        if (!strcmp(name, agent_target_name(i))) { *target = i; return true; }
    return false;
}

void agent_menu_reset(agent_menu_t *menu) {
    if (!menu) return;
    memset(menu, 0, sizeof(*menu));
    menu->pressed = -1;
}

void agent_menu_show(agent_menu_t *menu, bool online) {
    if (!menu) return;
    menu->open = true;
    agent_menu_visit(menu, AGENT_VIEW_OVERVIEW, AGENT_TARGET_AGENT);
    menu->half = 0;
    menu->touch_moved = false;
    menu->online = online;
    menu->pressed = -1;
    menu->pressed_t = 1.f;
    menu->error[0] = 0;
}

void agent_menu_hide(agent_menu_t *menu) {
    if (!menu) return;
    menu->open = false;
    menu->pressed = -1;
    menu->pressed_t = 1.f;
}

void agent_menu_set_online(agent_menu_t *menu, bool online) {
    if (!menu || menu->online == online) return;
    menu->online = online;
    menu->error[0] = 0;
    // Provider availability belongs to one authenticated session. Keep cached labels,
    // but require a fresh post-welcome capability frame before permitting capture.
    menu->capabilities_known = false;
    if (!online) menu->wire_rid = 0;
    if (!online && menu->request_pending) {
        menu->request_pending = false;
        menu->waiting_kind = AGENT_REQUEST_NONE;
        menu->retry_kind = AGENT_REQUEST_NONE;
        menu->retry_id[0] = 0;
    }
}

void agent_menu_set_capabilities(agent_menu_t *menu, bool stt_available, bool tts_available) {
    if (!menu) return;
    menu->capabilities_known = true;
    menu->stt_available = stt_available;
    menu->tts_available = tts_available;
}

static uint16_t begin_request(agent_menu_t *menu, agent_request_kind_t kind, uint32_t now_ms) {
    if (!menu || !menu->online || menu->wire_rid || kind == AGENT_REQUEST_NONE) return 0;
    uint16_t rid = (uint16_t)(menu->next_rid + 1u);
    if (!rid) rid = 1;
    menu->next_rid = menu->waiting_rid = rid;
    menu->request_pending = true;
    menu->waiting_kind = kind;
    menu->request_at_ms = menu->wire_at_ms = now_ms;
    menu->wire_rid = rid;
    menu->error[0] = 0;
    return rid;
}

uint16_t agent_menu_request_options(agent_menu_t *menu, uint8_t cursor, uint32_t now_ms) {
    uint16_t rid = begin_request(menu, AGENT_REQUEST_OPTIONS, now_ms);
    if (!rid) return 0;
    menu->waiting_cursor = menu->retry_cursor = cursor;
    menu->retry_kind = AGENT_REQUEST_OPTIONS;
    menu->retry_id[0] = 0;
    return rid;
}

uint16_t agent_menu_request_model(agent_menu_t *menu, const char *id, uint32_t now_ms) {
    if (!menu || !agent_menu_id_valid(id) || !menu->online) return 0;
    bool listed = false;
    for (unsigned i = 0; i < menu->count; i++) listed |= !menu->models[i].disabled && !strcmp(menu->models[i].id, id);
    if (!listed) return 0;
    uint16_t rid = begin_request(menu, AGENT_REQUEST_SELECT, now_ms);
    if (!rid || !copy_bounded(menu->retry_id, sizeof menu->retry_id, id)) return 0;
    menu->retry_kind = AGENT_REQUEST_SELECT;
    menu->waiting_cursor = menu->retry_cursor = menu->cursor;
    return rid;
}

uint16_t agent_menu_retry(agent_menu_t *menu, uint32_t now_ms) {
    if (!menu || !menu->online || menu->retry_kind == AGENT_REQUEST_NONE) return 0;
    if (menu->retry_kind == AGENT_REQUEST_SELECT && menu->retry_id[0])
        return agent_menu_request_model(menu, menu->retry_id, now_ms);
    return agent_menu_request_options(menu, menu->retry_cursor, now_ms);
}

static bool reply_catalog_valid(const agent_menu_reply_t *r) {
    if (!r->has_catalog) return true;
    unsigned first = (unsigned)r->cursor * AGENT_PAGE_SIZE;
    if (first > r->total || r->count > r->total - first) return false;
    for (unsigned i = 0; i < r->count; i++) {
        bool id_ok = agent_menu_id_valid(r->models[i].id);
        size_t n = strnlen(r->models[i].label, AGENT_MODEL_LABEL_MAX + 1);
        bool label_ok = n && n <= AGENT_MODEL_LABEL_MAX;
        for (size_t j = 0; label_ok && j < n; j++) {
            unsigned char c = (unsigned char)r->models[i].label[j];
            label_ok = c >= 0x20 && c != 0x7f;
        }
        if (!id_ok || !label_ok) return false;
    }
    return true;
}

static bool mode_catalog_valid(const agent_menu_reply_t *r) {
    if (r->target != AGENT_TARGET_MODE || !r->has_catalog) return true;
    if (r->cursor || r->count > 3 || r->total != r->count) return false;
    unsigned seen = 0;
    bool current = false;
    for (unsigned i = 0; i < r->count; ++i) {
        const char *id = r->models[i].id;
        unsigned bit = !strcmp(id, "classic") ? 1 : !strcmp(id, "realtime") ? 2 : !strcmp(id, "live") ? 4 : 0;
        if (!bit || (seen & bit)) return false;
        seen |= bit; current |= !strcmp(id, r->model);
    }
    return current;
}

static bool reply_is_valid(const agent_menu_reply_t *r) {
    if (!r || r->count > AGENT_PAGE_SIZE || r->total > 128 || !agent_menu_error_valid(r->error)) return false;
    if (r->has_model && r->model[0] && !agent_menu_id_valid(r->model)) return false;
    return reply_catalog_valid(r) && mode_catalog_valid(r);
}

static void accept_catalog(agent_menu_t *menu, const agent_menu_reply_t *reply) {
    if (!reply->has_catalog) return;
    menu->cursor = reply->cursor;
    menu->total = reply->total;
    menu->count = reply->count;
    if (menu->count <= 2) menu->half = 0;
    memcpy(menu->models, reply->models, sizeof menu->models);
    menu->options_loaded = true;
}

static void accept_status(agent_menu_t *menu, const agent_menu_reply_t *reply) {
    if (reply->has_stt) {
        menu->capabilities_known = true;
        menu->stt_available = reply->stt_available;
        copy_bounded(menu->stt_provider, sizeof menu->stt_provider, reply->stt_provider);
        copy_bounded(menu->stt_model, sizeof menu->stt_model, reply->stt_model);
    }
    if (reply->has_tts) {
        menu->capabilities_known = true;
        menu->tts_available = reply->tts_available;
        copy_bounded(menu->tts_provider, sizeof menu->tts_provider, reply->tts_provider);
        copy_bounded(menu->tts_model, sizeof menu->tts_model, reply->tts_model);
    }
}

static void accept_mode(agent_menu_t *menu, const agent_menu_reply_t *reply) {
    if (menu->target != AGENT_TARGET_MODE || !reply->has_model) return;
    if (!strcmp(reply->model, "classic")) menu->voice_mode = VOICE_CLASSIC;
    else if (!strcmp(reply->model, "realtime")) menu->voice_mode = VOICE_REALTIME;
    else if (!strcmp(reply->model, "live")) menu->voice_mode = VOICE_LIVE;
}

static void accept_error(agent_menu_t *menu, const agent_menu_reply_t *reply, agent_request_kind_t answered) {
    copy_bounded(menu->error, sizeof menu->error, reply->error);
    menu->retry_kind = !strcmp(reply->error, "unsupported") || !strcmp(reply->error, "denied")
        ? AGENT_REQUEST_NONE : answered;
    if (!strcmp(reply->error, "invalid_model")) {
        menu->retry_kind = AGENT_REQUEST_OPTIONS;
        menu->retry_cursor = menu->cursor;
        menu->retry_id[0] = 0;
    }
}

bool agent_menu_accept_reply(agent_menu_t *menu, const agent_menu_reply_t *reply, uint32_t now_ms) {
    if (!menu || !reply_is_valid(reply)) return false;
    // Navigation may abandon the page, but the host still owns its request lane.
    if (reply->rid == menu->wire_rid && reply->cursor == menu->waiting_cursor) menu->wire_rid = 0;
    if (!menu->request_pending || reply->rid != menu->waiting_rid) return false;
    agent_request_kind_t answered = menu->waiting_kind;
    if (reply->cursor != menu->waiting_cursor || reply->target != menu->target) return false;
    menu->request_pending = false;
    menu->waiting_kind = AGENT_REQUEST_NONE;
    if (reply->error[0]) { accept_error(menu, reply, answered); return true; }
    accept_catalog(menu, reply);
    if (reply->has_model) {
        copy_bounded(menu->selected_model, sizeof menu->selected_model, reply->model);
        if (menu->target == AGENT_TARGET_AGENT) copy_bounded(menu->model, sizeof menu->model, reply->model);
    }
    accept_mode(menu, reply);
    accept_status(menu, reply);
    menu->error[0] = 0;
    menu->retry_kind = AGENT_REQUEST_NONE;
    menu->retry_id[0] = 0;
    menu->request_at_ms = now_ms;
    return true;
}

bool agent_menu_timeout(agent_menu_t *menu, uint32_t now_ms) {
    if (menu && menu->wire_rid && (uint32_t)(now_ms - menu->wire_at_ms) >= AGENT_REQUEST_TIMEOUT_MS)
        menu->wire_rid = 0;
    if (!menu || !menu->request_pending || (uint32_t)(now_ms - menu->request_at_ms) < AGENT_REQUEST_TIMEOUT_MS)
        return false;
    menu->retry_kind = menu->waiting_kind;
    menu->request_pending = false;
    menu->waiting_kind = AGENT_REQUEST_NONE;
    copy_bounded(menu->error, sizeof menu->error, "timeout");
    return true;
}

bool agent_menu_current(const agent_menu_t *menu, unsigned index) {
    if (!menu || index >= menu->count) return false;
    const char *id = menu->target == AGENT_TARGET_AGENT ? menu->model : menu->selected_model;
    return id[0] && !strcmp(menu->models[index].id, id);
}

unsigned agent_menu_pages(const agent_menu_t *menu) {
    return menu ? (menu->total + AGENT_PAGE_SIZE - 1u) / AGENT_PAGE_SIZE : 0;
}

unsigned agent_menu_display_pages(const agent_menu_t *menu) {
    return menu ? (menu->total + 1u) / 2u : 0;
}

unsigned agent_menu_display_page(const agent_menu_t *menu) {
    return menu ? menu->cursor * 2u + menu->half : 0;
}

bool agent_menu_can_turn(const agent_menu_t *menu, bool next) {
    if (!menu || !menu->online || menu->request_pending) return false;
    unsigned page = agent_menu_display_page(menu);
    return next ? page + 1u < agent_menu_display_pages(menu) : page > 0;
}

bool agent_menu_turn_page(agent_menu_t *menu, bool next, uint8_t *cursor) {
    if (!cursor || !agent_menu_can_turn(menu, next)) return false;
    if (next && !menu->half) { menu->half = 1; return false; }
    if (!next && menu->half) { menu->half = 0; return false; }
    menu->half = next ? 0 : 1;
    *cursor = next ? menu->cursor + 1u : menu->cursor - 1u;
    return true;
}

bool agent_menu_drag(agent_menu_t *menu, int x, int y, bool first) {
    if (!menu || !menu->open) return false;
    if (first) {
        menu->touch_x = (int16_t)x; menu->touch_y = (int16_t)y;
        menu->touch_moved = false;
    } else {
        int dx = x - menu->touch_x, dy = y - menu->touch_y;
        if (dx * dx + dy * dy >= 24 * 24) menu->touch_moved = true;
    }
    return true;
}

bool agent_menu_take_tap(agent_menu_t *menu) {
    if (!menu) return false;
    bool allowed = !menu->touch_moved;
    menu->touch_moved = false;
    return allowed;
}

void agent_menu_start_guide(agent_menu_t *menu, bool online) {
    if (!menu) return;
    agent_menu_show(menu, online);
    menu->view = AGENT_VIEW_GUIDE;
    menu->guide_step = 0;
    menu->guide_save_failed = false;
    menu->guide_elapsed = 0;
}

void agent_menu_guide_move(agent_menu_t *menu, int direction) {
    if (!menu || menu->view != AGENT_VIEW_GUIDE) return;
    int next = menu->guide_step + direction;
    if (next < 0 || next > 3) return;
    menu->guide_step = (uint8_t)next;
    menu->guide_elapsed = 0;
}

void agent_menu_visit(agent_menu_t *m, agent_view_t view, agent_target_t target) {
    m->view = view; m->target = target;
    m->half = m->cursor = m->count = m->total = 0;
    m->selected_model[0] = m->error[0] = 0;
    m->request_pending = m->options_loaded = false;
    m->waiting_kind = m->retry_kind = AGENT_REQUEST_NONE;
    m->retry_id[0] = 0;
}

void agent_menu_voice_models(agent_menu_t *m) {
    agent_menu_visit(m, m->voice_mode == VOICE_CLASSIC ? AGENT_VIEW_CLASSIC : AGENT_VIEW_MODELS,
                     m->voice_mode == VOICE_CLASSIC ? AGENT_TARGET_MODE : AGENT_TARGET_VOICE);
}

void agent_menu_back(agent_menu_t *m) {
    if (m->view == AGENT_VIEW_OVERVIEW) { agent_menu_hide(m); return; }
    if (m->view == AGENT_VIEW_CLASSIC || m->target == AGENT_TARGET_VOICE)
        agent_menu_visit(m, AGENT_VIEW_VOICE_MODES, AGENT_TARGET_MODE);
    else if (m->target == AGENT_TARGET_STT || m->target == AGENT_TARGET_TTS)
        agent_menu_visit(m, AGENT_VIEW_CLASSIC, AGENT_TARGET_MODE);
    else agent_menu_visit(m, AGENT_VIEW_OVERVIEW, AGENT_TARGET_AGENT);
}
