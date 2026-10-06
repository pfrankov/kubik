#include "event_journal.h"
#include "ui_theme.h"
#include <stdio.h>
#include <string.h>

static void line(scene_t *s, text_role_t role, const char *text, float x, float y, float width, uint32_t color) {
    char shown[JOURNAL_TEXT + 4];
    size_t n = label_prefix(role, text, sizeof shown - 4, width, "...");
    memcpy(shown, text, n); shown[n] = 0;
    if (text[n]) strcat(shown, "...");
    sc_label(s, role, shown, x, y, -1, color, 1);
}
static void stamp(const journal_entry_t *e, char *text, size_t cap) {
    if (e->repeats > 1) snprintf(text, cap, "%lu:%02lu  x%u", (unsigned long)(e->seconds / 60), (unsigned long)(e->seconds % 60), e->repeats);
    else snprintf(text, cap, "%lu:%02lu", (unsigned long)(e->seconds / 60), (unsigned long)(e->seconds % 60));
}
static void header(const event_journal_t *j, scene_t *s) {
    sc_label(s, TXT_ACTION, "Events", 28, 36, -1, UI_TEXT, 1);
    sc_label(s, TXT_CAPTION, "Since restart", 452, 36, 1, UI_MUTED, 1);
    sc_rbox(s, 240, 92, 220, 28, 22, 0, UI_SURFACE, 1);
    sc_label(s, TXT_ACTION, "Overlay on home", 40, 92, -1, UI_TEXT, 1);
    sc_rbox(s, 416, 92, 26, 15, 15, 0, j->overlay ? UI_ACCENT : UI_DISABLED, 1);
    sc_circle(s, j->overlay ? 428 : 404, 92, 11, UI_INK, 1);
}
static size_t wrap_prefix(const char *text) {
    size_t n = label_prefix(TXT_CARD, text, JOURNAL_TEXT - 1, 424, "");
    if (text[n]) {
        size_t space = n;
        while (space && text[space] != ' ') space--;
        if (space) n = space;
    }
    return n;
}
static void detail(const event_journal_t *j, scene_t *s) {
    if (j->detail_expired) {
        sc_label(s, TXT_ACTION, "This event has expired", 240, 230, 0, UI_TEXT, 1);
        sc_label(s, TXT_CAPTION, "BOOT returns to recent events", 240, 278, 0, UI_MUTED, 1);
        return;
    }
    const journal_entry_t *e = event_journal_at(j, j->selected);
    if (!e) return;
    char time[32]; stamp(e, time, sizeof time);
    sc_label(s, TXT_CAPTION, event_journal_kind(e->kind), 28, 150, -1, UI_GOLD, 1);
    sc_label(s, TXT_CAPTION, time, 452, 150, 1, UI_MUTED, 1);
    const char *at = e->text;
    for (int i = 0; i < 7 && *at; i++) {
        size_t n = wrap_prefix(at);
        if (!n) break;
        char row[JOURNAL_TEXT]; memcpy(row, at, n); row[n] = 0;
        sc_label(s, TXT_CARD, row, 28, 188 + i * 28, -1, UI_TEXT, 1);
        at += n;
        while (*at == ' ') at++;
    }
    line(s, TXT_CAPTION, e->reaction, 28, 412, 424, UI_ACCENT);
    sc_label(s, TXT_CAPTION, "Message excerpt / BOOT returns", 240, 452, 0, UI_MUTED, 1);
}
void event_journal_draw(const event_journal_t *j, scene_t *s) {
    header(j, s);
    if (j->detail) { detail(j, s); return; }
    if (!j->count) {
        sc_label(s, TXT_ACTION, "No agent events yet", 240, 230, 0, UI_TEXT, 1);
        sc_label(s, TXT_CAPTION, "Connect or talk to your agent", 240, 278, 0, UI_MUTED, 1);
        return;
    }
    for (unsigned i = 0; i < 3; i++) {
        const journal_entry_t *e = event_journal_at(j, j->offset + i);
        if (!e) break;
        int top = 128 + i * 100;
        sc_rbox(s, 240, top + 46, 220, 46, 22, 0, UI_SURFACE, 1);
        char time[32]; stamp(e, time, sizeof time);
        sc_label(s, TXT_CAPTION, event_journal_kind(e->kind), 36, top + 17, -1, UI_GOLD, 1);
        sc_label(s, TXT_CAPTION, time, 444, top + 17, 1, UI_MUTED, 1);
        line(s, TXT_ACTION, e->text, 36, top + 45, 408, UI_TEXT);
        line(s, TXT_CAPTION, e->reaction, 36, top + 75, 408, UI_ACCENT);
    }
    sc_label(s, TXT_ACTION, "Newer", 80, 450, 0, UI_ACCENT, 1);
    sc_label(s, TXT_ACTION, "Older", 400, 450, 0, UI_GOLD, 1);
    char count[24]; snprintf(count, sizeof count, "%u / %u", j->offset + 1, j->count);
    sc_label(s, TXT_CAPTION, count, 240, 450, 0, UI_MUTED, 1);
}
void event_journal_overlay(const event_journal_t *j, scene_t *s, int top) {
    if (!j->overlay || !j->count) return;
    sc_rbox(s, 240, top + 44, 216, 44, 20, 0, UI_INK, .78f);
    const journal_entry_t *e = event_journal_at(j, 0);
    char row[JOURNAL_TEXT + 32];
    snprintf(row, sizeof row, "%s: %s", event_journal_kind(e->kind), e->text);
    line(s, TXT_CAPTION, row, 36, top + 22, 408, UI_GOLD);
    line(s, TXT_CAPTION, e->reaction, 36, top + 60, 408, UI_ACCENT);
}
