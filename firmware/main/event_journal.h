// A bounded, volatile journal of events applied by the device. No transport payloads.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "render.h"
#define JOURNAL_CAPACITY 16
#define JOURNAL_TEXT 160
typedef enum {
    JOURNAL_LINK, JOURNAL_STATE, JOURNAL_ACTIVITY, JOURNAL_EMOTION,
    JOURNAL_REPLY, JOURNAL_NOTICE, JOURNAL_SPEECH, JOURNAL_ERROR, JOURNAL_SETTINGS,
    JOURNAL_KIND_COUNT
} journal_kind_t;
typedef struct {
    uint32_t seconds;
    uint16_t repeats;
    uint8_t kind;
    char text[JOURNAL_TEXT];
    const char *reaction; // Borrowed immutable label; never message/transport storage.
} journal_entry_t;
typedef struct {
    journal_entry_t entries[JOURNAL_CAPACITY];
    uint8_t next, count, offset, selected;
    bool open, overlay, moved, detail, detail_expired;
    int16_t touch_x, touch_y, last_y;
    int drag;
} event_journal_t;
const char *event_journal_kind(int kind);
// Reaction labels must have static lifetime (all production callers use literals).
void event_journal_add(event_journal_t *j, uint32_t seconds, journal_kind_t kind, const char *text, const char *static_reaction);
const journal_entry_t *event_journal_at(const event_journal_t *j, unsigned age);
void event_journal_open(event_journal_t *j);
bool event_journal_back(event_journal_t *j);
bool event_journal_drag(event_journal_t *j, int x, int y, bool first);
// Returns true only when the overlay preference changes.
bool event_journal_tap(event_journal_t *j, int x, int y);
void event_journal_draw(const event_journal_t *j, scene_t *s);
void event_journal_overlay(const event_journal_t *j, scene_t *s, int top);
