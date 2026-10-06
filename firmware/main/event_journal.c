#include "event_journal.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>

_Static_assert(sizeof(event_journal_t) <= 3000, "journal must leave internal RAM for Wi-Fi and wake detection");

const char *event_journal_kind(int kind) {
    static const char *const names[] = {"Connection", "State", "Activity", "Emotion", "Reply", "Notification", "Speech", "Error", "Settings"};
    return kind >= 0 && kind < JOURNAL_KIND_COUNT ? names[kind] : "Event";
}
static unsigned utf8_length(unsigned char c) {
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) return 2;
    if (c >= 0xe0 && c <= 0xef) return 3;
    if (c >= 0xf0 && c <= 0xf4) return 4;
    return 0;
}
static bool utf8_valid(const char *in, unsigned len) {
    if (!len) return false;
    for (unsigned k = 1; k < len; k++)
        if (!in[k] || ((unsigned char)in[k] & 0xc0) != 0x80) return false;
    return true;
}
// Keep whole UTF-8 characters; flatten newlines/control bytes for compact rows.
static void copy_text(char *out, size_t cap, const char *in) {
    size_t n = 0;
    if (!in) in = "";
    while (*in && n + 1 < cap) {
        unsigned char c = (unsigned char)*in;
        unsigned len = utf8_length(c);
        bool valid = utf8_valid(in, len);
        if (!valid) { out[n++] = '?'; in++; continue; }
        if (n + len >= cap) break;
        if (c < 0x20 || c == 0x7f) out[n++] = ' ';
        else { memcpy(out + n, in, len); n += len; }
        in += len;
    }
    out[n] = 0;
}
const journal_entry_t *event_journal_at(const event_journal_t *j, unsigned age) {
    if (age >= j->count) return NULL;
    return &j->entries[(j->next + JOURNAL_CAPACITY - 1 - age) % JOURNAL_CAPACITY];
}
static void anchor(event_journal_t *j) {
    // Keep the item under the reader's finger anchored while newer events arrive.
    unsigned max = j->count > 3 ? j->count - 3 : 0;
    if (j->open && j->offset && j->offset < max) j->offset++;
    if (j->detail && !j->detail_expired) {
        if (j->selected + 1 < j->count) j->selected++;
        else j->detail_expired = true;
    }
}
void event_journal_add(event_journal_t *j, uint32_t seconds, journal_kind_t kind, const char *text, const char *static_reaction) {
    if (kind < 0 || kind >= JOURNAL_KIND_COUNT) return;
    journal_entry_t entry = {.seconds = seconds, .kind = kind, .repeats = 1};
    copy_text(entry.text, sizeof entry.text, text);
    entry.reaction = static_reaction ? static_reaction : "";
    const journal_entry_t *last = event_journal_at(j, 0);
    if (last && last->kind == kind && !strcmp(last->text, entry.text) && !strcmp(last->reaction, entry.reaction)) {
        journal_entry_t *mutable = &j->entries[(j->next + JOURNAL_CAPACITY - 1) % JOURNAL_CAPACITY];
        mutable->seconds = seconds;
        if (mutable->repeats < UINT16_MAX) mutable->repeats++;
        return;
    }
    j->entries[j->next] = entry;
    j->next = (j->next + 1) % JOURNAL_CAPACITY;
    if (j->count < JOURNAL_CAPACITY) j->count++;
    anchor(j);
}
void event_journal_open(event_journal_t *j) {
    j->open = true; j->offset = 0; j->detail = j->detail_expired = j->moved = false; j->drag = 0;
}
bool event_journal_back(event_journal_t *j) {
    if (!j->open) return false;
    if (j->detail) j->detail = false;
    else j->open = false;
    return true;
}
static void scroll(event_journal_t *j, int delta) {
    int offset = j->offset + delta;
    int max = j->count > 3 ? j->count - 3 : 0;
    j->offset = offset < 0 ? 0 : offset > max ? max : offset;
}
bool event_journal_drag(event_journal_t *j, int x, int y, bool first) {
    if (!j->open) return false;
    if (first) { j->touch_x = x; j->touch_y = j->last_y = y; j->moved = false; j->drag = 0; }
    else {
        int dx = x - j->touch_x, dy = y - j->touch_y;
        if (dx * dx + dy * dy > 64) j->moved = true;
        j->drag += j->last_y - y; j->last_y = y;
        if (!j->detail) {
            int rows = j->drag / 70;
            scroll(j, rows); j->drag -= rows * 70;
        }
    }
    return true;
}
bool event_journal_tap(event_journal_t *j, int x, int y) {
    if (!j->open || j->moved) { j->moved = false; return false; }
    if (y >= 64 && y <= 120) { j->overlay = !j->overlay; return true; }
    if (j->detail) return false;
    if (y >= 128 && y < 428 && j->offset + (y - 128) / 100 < j->count) {
        j->selected = j->offset + (y - 128) / 100; j->detail = true; j->detail_expired = false;
    } else if (y >= 428 && y <= 480) scroll(j, x < 240 ? -3 : 3);
    return false;
}
