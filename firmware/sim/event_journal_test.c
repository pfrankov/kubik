#include "../main/event_journal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void history(void) {
    event_journal_t j = {0};
    assert(!event_journal_at(&j, 0));
    event_journal_add(&j, 10, JOURNAL_REPLY, "Hello\nthere\t!", "Text card");
    assert(!strcmp(event_journal_at(&j, 0)->text, "Hello there !"));
    event_journal_add(&j, 11, JOURNAL_REPLY, "Hello there !", "Text card");
    assert(j.count == 1 && event_journal_at(&j, 0)->repeats == 2);
    for (unsigned i = 0; i < 40; i++) {
        char text[20]; snprintf(text, sizeof text, "Event %u", i);
        event_journal_add(&j, i + 12, JOURNAL_ACTIVITY, text, "Reaction");
    }
    assert(j.count == JOURNAL_CAPACITY);
    assert(!strcmp(event_journal_at(&j, 0)->text, "Event 39"));
    char oldest[20]; snprintf(oldest, sizeof oldest, "Event %u", 40 - JOURNAL_CAPACITY);
    assert(!strcmp(event_journal_at(&j, JOURNAL_CAPACITY - 1)->text, oldest));
    assert(!event_journal_at(&j, JOURNAL_CAPACITY));
    event_journal_open(&j);
    event_journal_tap(&j, 410, 450); assert(j.offset == 3);
    char anchored[JOURNAL_TEXT]; strcpy(anchored, event_journal_at(&j, j.offset)->text);
    event_journal_add(&j, 100, JOURNAL_ERROR, "Failed", "Error");
    assert(j.offset == 4 && !strcmp(anchored, event_journal_at(&j, j.offset)->text));
    event_journal_tap(&j, 200, 173); assert(j.detail);
    assert(event_journal_back(&j) && j.open && !j.detail);
    assert(event_journal_back(&j) && !j.open);
    event_journal_open(&j); j.detail = true; j.selected = j.count - 2;
    event_journal_add(&j, 101, JOURNAL_ERROR, "Next", "Error");
    assert(j.selected == j.count - 1 && !j.detail_expired);
    event_journal_add(&j, 102, JOURNAL_ERROR, "Latest", "Error");
    assert(j.detail_expired); // Never silently show a different message when the read entry is evicted.
    event_journal_open(&j); assert(!j.detail_expired);
}
static void gestures(void) {
    event_journal_t j = {0};
    for (int i = 0; i < 12; i++) event_journal_add(&j, i, i % JOURNAL_KIND_COUNT, "Entry", "Reaction");
    event_journal_open(&j);
    assert(event_journal_tap(&j, 250, 90) && j.overlay);
    event_journal_drag(&j, 240, 360, true);
    event_journal_drag(&j, 240, 80, false); assert(j.offset == 4);
    assert(!event_journal_tap(&j, 250, 80) && j.overlay && !j.detail);
    event_journal_drag(&j, 240, 90, true);
    assert(!event_journal_tap(&j, 250, 200) && j.detail);
    event_journal_drag(&j, 240, 300, true); event_journal_drag(&j, 240, 60, false);
    assert(j.offset == 4); // Reading details must not silently change the selected message.
    event_journal_back(&j);
    for (int i = 0; i < 20; i++) event_journal_tap(&j, 400, 450);
    assert(j.offset == j.count - 3);
    for (int i = 0; i < 20; i++) event_journal_tap(&j, 70, 450);
    assert(j.offset == 0);
    event_journal_drag(&j, 240, 90, true); event_journal_drag(&j, 251, 90, false);
    assert(!event_journal_tap(&j, 251, 90) && j.overlay); // Horizontal drag is not a toggle.
    event_journal_drag(&j, 240, 90, true); event_journal_drag(&j, 240, 102, false);
    assert(!event_journal_tap(&j, 240, 102) && j.overlay); // Short vertical drag is not a toggle.
    event_journal_add(&j, 200, JOURNAL_ERROR, "New", "Error"); assert(j.offset == 0);
}
static void strings(void) {
    event_journal_t j = {0};
    char text[200]; memset(text, 'a', sizeof text); text[158] = '\xc3'; text[159] = '\xa9'; text[160] = 0;
    event_journal_add(&j, 1, JOURNAL_REPLY, text, NULL);
    assert(strlen(event_journal_at(&j, 0)->text) == 158);
    event_journal_add(&j, 2, JOURNAL_REPLY, "\xff\xc3", "");
    assert(!strcmp(event_journal_at(&j, 0)->text, "??"));
    event_journal_add(&j, 3, JOURNAL_KIND_COUNT, "Bad", "Ignored"); assert(j.count == 2);
}
int main(void) { history(); gestures(); strings(); puts("journal: bounded history, deduplication, scroll anchoring, tap/drag, details and UTF-8 passed"); }
