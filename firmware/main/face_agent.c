#include "face_agent.h"
#include "font.h"
#include "ui_theme.h"
#include <stdio.h>
#include <string.h>

#define TILE UI_SURFACE
#define PRESSED UI_PRESSED
#define SELECTED UI_SELECTED
#define ACCENT UI_ACCENT
#define CORAL UI_ERROR
#define MUTED UI_MUTED
#define TEXT UI_TEXT
#define DISABLED UI_DISABLED

static void fit_text(char *out, size_t cap, const char *src, text_role_t role, float max_width) {
    if (!cap) return;
    src = src ? src : "";
    size_t n = label_prefix(role, src, cap - 1, max_width, "");
    const char *suffix = "";
    if (src[n] && cap >= 4) {
        n = label_prefix(role, src, cap - 4, max_width, "...");
        suffix = "...";
    }
    memcpy(out, src, n);
    strcpy(out + n, suffix);
}

// Geometry follows the main settings: 20px outer margin, 12px gaps, rounded tiles.
static void tile(scene_t *s, const agent_menu_t *m, face_agent_hit_t hit,
                 float x, float y, float hw, float hh, uint32_t color) {
    bool pressed = m->pressed == (int)hit && m->pressed_t < 0.24f;
    uint32_t lit = color == ACCENT ? UI_ACCENT_PRESSED : color == UI_GOLD ? UI_GOLD_PRESSED : PRESSED;
    sc_rbox(s, x, y, hw, hh, UI_RADIUS, 0, pressed ? lit : color, 1);
}
static void fitted(scene_t *s, const char *text, text_role_t role, float x, float y, int align,
                   float width, uint32_t color) {
    char line[AGENT_MODEL_ID_MAX * 2 + 5];
    fit_text(line, sizeof line, text, role, width);
    sc_label(s, role, line, x, y, align, color, 1);
}
static void chevron(scene_t *s, float x, float y, bool next, uint32_t color) {
    float d = next ? 1 : -1;
    sc_capsule(s, x - d * 5, y - 8, x + d * 3, y, 2.5f, color, 1);
    sc_capsule(s, x + d * 3, y, x - d * 5, y + 8, 2.5f, color, 1);
}
static const char *view_title(const agent_menu_t *m) {
    switch (m->view) {
    case AGENT_VIEW_MODELS: return m->target == AGENT_TARGET_AGENT ? "Agent model" :
        m->target == AGENT_TARGET_VOICE ? (m->voice_mode == VOICE_LIVE ? "GPT Live" : "Realtime") :
        m->target == AGENT_TARGET_STT ? "STT model" : "TTS model";
    case AGENT_VIEW_VOICE_MODES: return "Voice";
    case AGENT_VIEW_CLASSIC: return "STT";
    default: return "Agent";
    }
}
static const char *error_message(const char *error) {
    if (!strcmp(error, "busy")) return "Busy";
    if (!strcmp(error, "timeout")) return "Timed out";
    if (!strcmp(error, "unsupported")) return "Unavailable";
    if (!strcmp(error, "unavailable")) return "Unavailable";
    if (!strcmp(error, "invalid_model")) return "Refresh";
    if (!strcmp(error, "denied")) return "Denied";
    return "";
}
static const char *screen_status(const agent_menu_t *m) {
    if (!m->online) return "Offline";
    if (m->request_pending) return m->waiting_kind == AGENT_REQUEST_SELECT ? "Saving" : "Updating";
    if (m->error[0]) return error_message(m->error);
    if (m->wire_rid) return "Updating";
    return "";
}
static void header(scene_t *s, const agent_menu_t *m) {
    sc_label(s, TXT_PAGE_TITLE, view_title(m), 40, 42, -1, TEXT, 1);
    const char *status = screen_status(m);
    if (status[0]) fitted(s, status, TXT_CHOICE, 440, 42, 1, 138, m->error[0] || !m->online ? CORAL : MUTED);
    else sc_icon(s, ICON_CHECK, 426, 42, 32, ACCENT, 1);
}
static const char *model_name(const agent_menu_t *m) {
    for (unsigned i = 0; m->target == AGENT_TARGET_AGENT && i < m->count; i++) if (agent_menu_current(m, i)) return m->models[i].label;
    const char *slash = strchr(m->model, '/');
    return slash ? slash + 1 : m->model;
}
static void model_provider(char *out, size_t cap, const char *id) {
    const char *slash = strchr(id, '/');
    size_t n = slash ? (size_t)(slash - id) : 0;
    if (n >= cap) n = cap - 1;
    memcpy(out, id, n); out[n] = 0;
}
static bool capability_ready(const agent_menu_t *m, bool stt) {
    return m->capabilities_known && (stt ? m->stt_available : m->tts_available);
}
static const char *capability_status(const agent_menu_t *m, bool stt) {
    if (!m->capabilities_known) return "Unknown";
    return capability_ready(m, stt) ? "Ready" : "Not set up";
}
static uint32_t capability_color(const agent_menu_t *m, bool stt) {
    return !m->capabilities_known ? MUTED : !capability_ready(m, stt) ? CORAL : stt ? ACCENT : UI_GOLD;
}
static const char *voice_name(voice_mode_t mode) {
    static const char *const names[] = {"STT", "Realtime", "GPT Live"};
    return names[mode];
}
static void overview_voice(scene_t *s, const agent_menu_t *m) {
    tile(s, m, AGENT_HIT_VOICE, 240, 350, 220, 88, TILE);
    sc_icon(s, ICON_MIC, 62, 294, 30, ACCENT, 1);
    sc_label(s, TXT_CHOICE, "Voice", 92, 294, -1, MUTED, 1);
    sc_label(s, TXT_PAGE_BODY, voice_name(m->voice_mode), 44, 344, -1, TEXT, 1);
    fitted(s, capability_status(m, true), TXT_CHOICE, 44, 398, -1, 352, capability_color(m, true));
    chevron(s, 428, 344, true, ACCENT);
}
static void overview(scene_t *s, const agent_menu_t *m) {
    tile(s, m, AGENT_HIT_MODELS, 240, 168, 220, 80, TILE);
    sc_icon(s, ICON_BLOCKS, 62, 120, 30, UI_GOLD, 1);
    sc_label(s, TXT_CHOICE, "Agent model", 92, 120, -1, MUTED, 1);
    const char *name = m->model[0] ? model_name(m) : m->online ? "Loading..." : "Unknown";
    fitted(s, name, TXT_PAGE_BODY, 44, 169, -1, 352, TEXT);
    char provider[AGENT_MODEL_ID_MAX + 1]; model_provider(provider, sizeof provider, m->model);
    fitted(s, provider, TXT_CHOICE, 44, 216, -1, 392, MUTED);
    chevron(s, 428, 169, true, UI_GOLD);
    overview_voice(s, m);
}
static void classic_setting(scene_t *s, const agent_menu_t *m, bool stt) {
    float y = stt ? 214 : 366;
    face_agent_hit_t hit = stt ? AGENT_HIT_STT : AGENT_HIT_TTS;
    tile(s, m, hit, 240, y, 220, 64, TILE);
    sc_icon(s, stt ? ICON_MIC : ICON_VOLUME_2, 62, y - 28, 30, stt ? ACCENT : UI_GOLD, 1);
    sc_label(s, TXT_PAGE_BODY, stt ? "Recognition" : "Speech", 94, y - 28, -1, TEXT, 1);
    const char *name = stt ? m->stt_model : m->tts_model;
    if (!capability_ready(m, stt)) name = stt ? "Not set up" : "Text replies";
    fitted(s, name[0] ? name : "Agent default", TXT_CHOICE, 44, y + 28, -1, 352, MUTED);
    chevron(s, 428, y + 28, true, stt ? ACCENT : UI_GOLD);
}
static void classic_settings(scene_t *s, const agent_menu_t *m) {
    sc_label(s, TXT_CHOICE, "Transcribe, then reply", 40, 108, -1, MUTED, 1);
    classic_setting(s, m, true); classic_setting(s, m, false);
}
static void model_heading(scene_t *s, const char *name, float y, uint32_t color) {
    if (label_width(TXT_PAGE_BODY, name) <= 352) {
        sc_label(s, TXT_PAGE_BODY, name, 44, y - 18, -1, color, 1); return;
    }
    char first[AGENT_MODEL_LABEL_MAX + 1];
    const char *end = name + label_prefix(TXT_PAGE_BODY, name, sizeof first - 1, 352, "");
    const char *space = NULL;
    for (const char *p = name; p < end; p++) if (*p == ' ') space = p;
    if (space) end = space;
    size_t n = (size_t)(end - name);
    memcpy(first, name, n); first[n] = 0;
    while (*end == ' ') end++;
    sc_label(s, TXT_PAGE_BODY, first, 44, y - 30, -1, color, 1);
    fitted(s, end, TXT_PAGE_BODY, 44, y + 8, -1, 352, color);
}
static void model_row(scene_t *s, const agent_menu_t *m, unsigned i, float y) {
    bool selected = agent_menu_current(m, i);
    bool saving = m->request_pending && m->waiting_kind == AGENT_REQUEST_SELECT && !strcmp(m->retry_id, m->models[i].id);
    tile(s, m, (face_agent_hit_t)(AGENT_HIT_MODEL_0 + i), 240, y, 220, 70, selected ? SELECTED : TILE);
    uint32_t text = selected ? UI_INK : m->online ? TEXT : MUTED;
    uint32_t detail = selected ? UI_INK : MUTED;
    model_heading(s, m->models[i].label, y, text);
    char provider[AGENT_MODEL_ID_MAX + 1]; model_provider(provider, sizeof provider, m->models[i].id);
    fitted(s, saving ? "Saving..." : provider, TXT_CHOICE, 44, y + 48, -1, 352, saving ? ACCENT : detail);
    if (selected) sc_icon(s, ICON_CHECK, 426, y - 12, 30, UI_INK, 1);
}
static bool show_retry(const agent_menu_t *m) {
    return m->online && m->error[0] && m->retry_kind != AGENT_REQUEST_NONE && !m->request_pending;
}
static void navigation(scene_t *s, const agent_menu_t *m) {
    if (show_retry(m)) {
        tile(s, m, AGENT_HIT_RETRY, 240, 428, 220, 32, TILE);
        sc_label(s, TXT_CHOICE, "Retry", 240, 428, 0, ACCENT, 1); return;
    }
    unsigned pages = agent_menu_display_pages(m);
    if (!pages) {
        if (!m->online || m->request_pending) return;
        tile(s, m, AGENT_HIT_REFRESH, 240, 428, 220, 32, TILE);
        sc_label(s, TXT_CHOICE, "Refresh", 240, 428, 0, ACCENT, 1); return;
    }
    bool prev = agent_menu_can_turn(m, false), next = agent_menu_can_turn(m, true);
    tile(s, m, AGENT_HIT_PREV, 88, 428, 68, 32, TILE);
    tile(s, m, AGENT_HIT_NEXT, 392, 428, 68, 32, TILE);
    chevron(s, 88, 428, false, prev ? TEXT : DISABLED);
    chevron(s, 392, 428, true, next ? TEXT : DISABLED);
    char page[24]; snprintf(page, sizeof page, "%u / %u", agent_menu_display_page(m) + 1u, pages);
    sc_label(s, TXT_CHOICE, page, 240, 428, 0, MUTED, 1);
}
static const char *empty_voice_hint(const agent_menu_t *m) {
    if (!m->online) return "";
    if (!m->capabilities_known) return "Checking voice...";
    bool stt = m->target != AGENT_TARGET_TTS;
    if (capability_ready(m, stt)) return "Voice is set on your agent";
    if (m->voice_mode != VOICE_CLASSIC) return "Set up voice on your agent";
    return stt ? "Set up STT on your agent" : "Replies will appear as text";
}
static bool models_loading(const agent_menu_t *m) {
    return m->online && !m->error[0] && (!m->options_loaded ||
        (m->request_pending && m->waiting_kind == AGENT_REQUEST_OPTIONS));
}
static void models(scene_t *s, const agent_menu_t *m) {
    bool loading = models_loading(m);
    if (!m->count || loading) {
        sc_icon(s, ICON_BRAIN, 240, 190, 48, ACCENT, 1);
        const char *line = loading ? (m->view == AGENT_VIEW_VOICE_MODES ? "Loading modes..." : "Loading models...") : !m->online ? "Connect your agent" : "No selectable models";
        fitted(s, line, TXT_PAGE_BODY, 240, 260, 0, 404, TEXT);
        if (!loading && m->target != AGENT_TARGET_AGENT) {
            fitted(s, empty_voice_hint(m), TXT_CHOICE, 240, 312, 0, 408, MUTED);
        }
    } else {
        unsigned first = m->half * 2u;
        for (unsigned j = 0; j < 2 && first + j < m->count; j++) model_row(s, m, first + j, 158 + j * 152);
    }
    navigation(s, m);
}
static const char *mode_hint(const agent_model_t *choice) {
    if (choice->disabled) return "Set up on your agent";
    if (!strcmp(choice->id, "classic")) return "Transcribe, then reply";
    if (!strcmp(choice->id, "realtime")) return "Stream voice with your agent";
    return "Continuous conversation";
}
static int mode_height(const agent_menu_t *m) { return show_retry(m) ? 92 : 112; }
static void mode_row(scene_t *s, const agent_menu_t *m, unsigned i) {
    int height = mode_height(m);
    float y = 88 + height / 2 + i * (height + 12);
    bool selected = agent_menu_current(m, i);
    uint32_t ink = selected ? UI_INK : TEXT;
    tile(s, m, (face_agent_hit_t)(AGENT_HIT_MODEL_0 + i), 240, y, 220, height / 2, selected ? SELECTED : TILE);
    fitted(s, m->models[i].label, TXT_PAGE_BODY, 44, y - 18, -1, 352, ink);
    bool saving = m->request_pending && m->waiting_kind == AGENT_REQUEST_SELECT && !strcmp(m->retry_id, m->models[i].id);
    fitted(s, saving ? "Saving..." : mode_hint(&m->models[i]), TXT_CHOICE, 44, y + 23, -1, 368, selected ? UI_INK : MUTED);
    if (selected) sc_icon(s, ICON_CHECK, 426, y - 18, 30, UI_INK, 1);
    else if (!m->models[i].disabled) chevron(s, 428, y - 18, true, ACCENT);
}
static void voice_modes(scene_t *s, const agent_menu_t *m) {
    if (!m->count || models_loading(m)) { models(s, m); return; }
    for (unsigned i = 0; i < m->count && i < 3; ++i) mode_row(s, m, i);
    if (show_retry(m)) navigation(s, m);

}
static void guide_line(scene_t *s, const char *text, float y, uint32_t color) {
    fitted(s, text, TXT_PAGE_BODY, 40, y, -1, 400, color);
}
static void guide_hint(scene_t *s, const char *text, float y) {
    fitted(s, text, TXT_CHOICE, 40, y, -1, 400, MUTED);
}
static void guide_button_cue(scene_t *s, const agent_menu_t *m) {
    enum { BOOT, PWR, KEY } button;
    if (m->guide_step == 0 || m->guide_step == 3) button = BOOT;
    else if (m->guide_step == 1 && m->online && capability_ready(m, true)) button = KEY;
    else return;
    // Front-view order. Provisional top-edge positions; tune these to the enclosure.
    // PWR has no instruction in this tour, so it is never highlighted here.
    static const struct { const char *name; float x, y; } anchors[] = {
        [BOOT] = {"BOOT", 140, 8}, [PWR] = {"PWR", 240, 8}, [KEY] = {"KEY", 332, 8},
    };
    const char *name = anchors[button].name;
    float x = anchors[button].x, y = anchors[button].y;
    float side = x < 240 ? -1 : 1;
    float start = 240 + side * (label_width(TXT_CHOICE, name) * .5f + 12);
    sc_label(s, TXT_CHOICE, name, 240, 42, 0, UI_GOLD, 1);
    sc_capsule(s, start, 42, x, 42, 2, UI_GOLD, 1);
    sc_capsule(s, x, 42, x, y, 2, UI_GOLD, 1);
    sc_capsule(s, x - 8, y + 9, x, y, 2, UI_GOLD, 1);
    sc_capsule(s, x, y, x + 8, y + 9, 2, UI_GOLD, 1);
}
static void guide_voice_missing(scene_t *s, const agent_menu_t *m, bool stt, bool known, float dy) {
    if (!known) {
        guide_line(s, m->online ? "Checking voice..." : "Agent is offline", 216 + dy, TEXT);
        guide_hint(s, "Connect, then check again", 260 + dy);
    } else {
        guide_line(s, stt ? "Voice input isn't set up" : "Replies will use text", 216 + dy, TEXT);
        guide_hint(s, m->voice_mode != VOICE_CLASSIC ? "Set up voice on your agent" :
                   stt ? "Enable STT on your agent" : "Enable TTS on your agent", 260 + dy);
    }
    tile(s, m, AGENT_HIT_REFRESH, 240, 356, 220, 28, TILE);
    sc_label(s, TXT_CHOICE, m->request_pending ? "Checking..." : "Check again", 240, 356, 0,
             m->online ? ACCENT : MUTED, 1);
}
static void guide_voice_input(scene_t *s, const agent_menu_t *m, float dy, bool voice_wake) {
    if (m->voice_mode == VOICE_LIVE) {
        guide_line(s, voice_wake ? "Say 'Hi Tessa' or press KEY." : "Press KEY, then talk.", 216 + dy, TEXT);
        guide_line(s, "Keep talking after replies.", 260 + dy, TEXT);
        guide_hint(s, "Press KEY to end GPT Live.", 316 + dy);
        return;
    }
    bool native = m->voice_mode != VOICE_CLASSIC;
    const char *start = voice_wake ? "Home: 'Hi Tessa', then talk." : native ? "Press KEY, then talk." : "Hold KEY to talk.";
    const char *hint = voice_wake ? (native ? "KEY also works." : "Or hold KEY; release to send.") :
                       native ? "KEY also ends recording." : "Mic is off in between.";
    guide_line(s, start, 216 + dy, TEXT);
    guide_line(s, voice_wake || native ? "Pause to send." : "Release to send.", 260 + dy, TEXT);
    guide_hint(s, hint, 316 + dy);
}
static void guide_voice(scene_t *s, const agent_menu_t *m, bool stt, float dy, bool voice_wake) {
    bool known = m->online && m->capabilities_known;
    bool ready = known && capability_ready(m, stt);
    if (!ready) { guide_voice_missing(s, m, stt, known, dy); return; }
    if (stt) { guide_voice_input(s, m, dy, voice_wake); return; }
    guide_line(s, m->volume >= 20 ? "Voice replies are ready" : "Volume is below 20", 216 + dy, TEXT);
    guide_line(s, "Volume 20+ for voice.", 260 + dy, TEXT);
    guide_hint(s, "Lower volume for text.", 316 + dy);
}
static void guide_heading(scene_t *s, const agent_menu_t *m, float dy) {
    static const char *const titles[] = {"Hello, I'm Kubik", "Talk to your agent", "Replies your way", "You're ready"};
    static const int icons[] = {ICON_AUDIO_LINES, ICON_MIC, ICON_VOLUME_2, ICON_CHECK};
    uint32_t color = m->guide_step == 1 || m->guide_step == 3 ? ACCENT : UI_GOLD;
    if (m->guide_step == 1 || m->guide_step == 2) {
        bool known = m->online && m->capabilities_known;
        if (!known || !capability_ready(m, m->guide_step == 1)) color = known ? CORAL : MUTED;
    }
    sc_icon(s, icons[m->guide_step], 68, 120 + dy, 56, color, 1);
    fitted(s, titles[m->guide_step], TXT_PAGE_BODY, 116, 120 + dy, -1, 324, TEXT);
}
static void guide_content(scene_t *s, const agent_menu_t *m, float dy, bool voice_wake) {
    guide_heading(s, m, dy);
    if (m->guide_step == 1 || m->guide_step == 2) { guide_voice(s, m, m->guide_step == 1, dy, voice_wake); return; }
    if (m->guide_step == 0) {
        guide_line(s, m->online ? "Your agent is connected" : "Agent is offline", 216 + dy, TEXT);
        guide_line(s, "Press BOOT to go back.", 272 + dy, TEXT);
        guide_hint(s, "Hold BOOT to open/close Settings.", 316 + dy);
    } else {
        // Explain the two real destinations without a pretend interactive tile.
        sc_icon(s, ICON_BRAIN, 64, 220 + dy, 40, ACCENT, 1);
        sc_label(s, TXT_PAGE_BODY, "Agent", 104, 204 + dy, -1, TEXT, 1);
        sc_label(s, TXT_CHOICE, "Model and voice", 104, 240 + dy, -1, MUTED, 1);
        sc_icon(s, ICON_CIRCLE_HELP, 64, 300 + dy, 40, UI_GOLD, 1);
        sc_label(s, TXT_PAGE_BODY, "Guide", 104, 284 + dy, -1, TEXT, 1);
        sc_label(s, TXT_CHOICE, "Replay this tour", 104, 320 + dy, -1, MUTED, 1);
        guide_hint(s, "Hold BOOT to open/close Settings.", 364 + dy);
    }
}
static void guide(scene_t *s, const agent_menu_t *m, bool voice_wake) {
    if (m->guide_step > 3) return;
    tile(s, m, AGENT_HIT_GUIDE_SKIP, 408, 42, 52, 30, TILE);
    sc_label(s, TXT_CHOICE, "Skip", 408, 42, 0, MUTED, 1);
    for (unsigned i = 0; i < 4; i++) sc_circle(s, 44 + i * 22, 42, i == m->guide_step ? 5 : 3,
        i == m->guide_step ? UI_GOLD : DISABLED, 1);
    int first = s->n;
    float t = m->guide_elapsed / 0.18f;
    if (t > 1) t = 1;
    if (t < 0) t = 0;
    float ease = 1 - (1 - t) * (1 - t) * (1 - t);
    guide_button_cue(s, m);
    guide_content(s, m, 10 * (1 - ease), voice_wake);
    for (int i = first; i < s->n; i++) s->p[i].alpha = (uint8_t)(s->p[i].alpha * ease);
    face_agent_hit_t hit = m->guide_step == 3 ? AGENT_HIT_GUIDE_DONE : AGENT_HIT_GUIDE_NEXT;
    tile(s, m, hit, 240, 428, 220, 32, UI_GOLD);
    sc_label(s, TXT_CHOICE, m->guide_save_failed ? "Couldn't save. Try again" :
             m->guide_step == 3 ? "Open settings" : m->guide_step ? "Next" : "Let's go",
             240, 428, 0, UI_INK, 1);
}
void face_agent_draw(scene_t *s, const agent_menu_t *m, bool is_tess) {
    if (!s || !m || !m->open) return;
    bool voice_wake = is_tess;
    if (m->view == AGENT_VIEW_GUIDE) { guide(s, m, voice_wake); return; }
    header(s, m);
    switch (m->view) {
    case AGENT_VIEW_MODELS: models(s, m); break;
    case AGENT_VIEW_VOICE_MODES: voice_modes(s, m); break;
    case AGENT_VIEW_CLASSIC: classic_settings(s, m); break;
    default: overview(s, m); break;
    }
}
static bool inside(int x, int y, int x0, int y0, int x1, int y1) {
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}
static face_agent_hit_t hit_model_navigation(const agent_menu_t *m, int x, int y) {
    if (!inside(x, y, 20, 396, 460, 460)) return AGENT_HIT_NONE;
    if (show_retry(m)) return AGENT_HIT_RETRY;
    if (!m->count) return AGENT_HIT_REFRESH;
    if (x <= 156 && agent_menu_can_turn(m, false)) return AGENT_HIT_PREV;
    if (x >= 324 && agent_menu_can_turn(m, true)) return AGENT_HIT_NEXT;
    return AGENT_HIT_NONE;
}
static face_agent_hit_t hit_models(const agent_menu_t *m, int x, int y) {
    if (!m->online || m->request_pending) return AGENT_HIT_NONE;
    face_agent_hit_t nav = hit_model_navigation(m, x, y);
    if (nav != AGENT_HIT_NONE) return nav;
    unsigned i = m->half * 2u;
    if (inside(x, y, 20, 88, 460, 228) && i < m->count) return (face_agent_hit_t)(AGENT_HIT_MODEL_0 + i);
    if (inside(x, y, 20, 240, 460, 380) && i + 1u < m->count) return (face_agent_hit_t)(AGENT_HIT_MODEL_0 + i + 1u);
    return AGENT_HIT_NONE;
}
static face_agent_hit_t hit_overview(int x, int y) {
    if (inside(x, y, 20, 88, 460, 248)) return AGENT_HIT_MODELS;
    if (inside(x, y, 20, 262, 460, 438)) return AGENT_HIT_VOICE;
    return AGENT_HIT_NONE;
}
static face_agent_hit_t hit_voice_modes(const agent_menu_t *m, int x, int y) {
    if (!m->online || m->request_pending) return AGENT_HIT_NONE;
    if (show_retry(m) && inside(x, y, 20, 396, 460, 460)) return AGENT_HIT_RETRY;
    if (!m->count) return hit_model_navigation(m, x, y);
    for (unsigned i = 0; i < m->count && i < 3; ++i) {
        if (inside(x, y, 20, 88 + i * (mode_height(m) + 12), 460, 88 + i * (mode_height(m) + 12) + mode_height(m)) &&
            (!m->models[i].disabled || agent_menu_current(m, i))) return (face_agent_hit_t)(AGENT_HIT_MODEL_0 + i);
    }
    return AGENT_HIT_NONE;
}
static face_agent_hit_t hit_classic(const agent_menu_t *m, int x, int y) {
    if (!m->online || m->request_pending) return AGENT_HIT_NONE;
    if (inside(x, y, 20, 150, 460, 278)) return AGENT_HIT_STT;
    if (inside(x, y, 20, 302, 460, 430)) return AGENT_HIT_TTS;
    return AGENT_HIT_NONE;
}
static face_agent_hit_t hit_guide(const agent_menu_t *m, int x, int y) {
    if (inside(x, y, 356, 12, 460, 72)) return AGENT_HIT_GUIDE_SKIP;
    if (inside(x, y, 20, 396, 460, 460)) return m->guide_step == 3 ? AGENT_HIT_GUIDE_DONE : AGENT_HIT_GUIDE_NEXT;
    if (inside(x, y, 20, 328, 460, 384) && (m->guide_step == 1 || m->guide_step == 2) &&
            m->online && !m->request_pending && (!m->capabilities_known || !capability_ready(m, m->guide_step == 1)))
            return AGENT_HIT_REFRESH;
    return AGENT_HIT_NONE;
}
face_agent_hit_t face_agent_hit(const agent_menu_t *m, int x, int y) {
    if (!m || !m->open || !inside(x, y, 20, 0, 460, 479)) return AGENT_HIT_NONE;
    if (m->view == AGENT_VIEW_GUIDE) return hit_guide(m, x, y);
    switch (m->view) {
    case AGENT_VIEW_MODELS: return hit_models(m, x, y);
    case AGENT_VIEW_VOICE_MODES: return hit_voice_modes(m, x, y);
    case AGENT_VIEW_CLASSIC: return hit_classic(m, x, y);
    case AGENT_VIEW_OVERVIEW: return hit_overview(x, y);
    default: return AGENT_HIT_NONE;
    }
}
