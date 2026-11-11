#include "face_internal.h"
#include "emoji_cluster.h"

static bool key_add(face_emoji_key_t keys[4], unsigned *count, const char *text, size_t bytes) {
    for (unsigned i = 0; i < *count; i++)
        if (keys[i].bytes == bytes && !memcmp(keys[i].text, text, bytes)) return true;
    if (*count == R_EMOJI_SLOTS) return false;
    keys[(*count)++] = (face_emoji_key_t){text, bytes};
    return true;
}

bool face_emoji_collect(const char *text, size_t bytes, face_emoji_key_t keys[4], unsigned *count) {
    while (bytes) {
        bool emoji;
        size_t step = emoji_cluster_next(text, bytes, &emoji);
        if (!step) return false;
        if (emoji && !key_add(keys, count, text, step)) return false;
        text += step;
        bytes -= step;
    }
    return true;
}

static unsigned page_keys(const face_t *f, char keys[4][65]) {
    face_emoji_key_t refs[4];
    unsigned count = 0, usable = 0;
    if (!f->card_n || f->card_page >= f->card_pages) return 0;
    const char *line = f->card_buf + f->card_page_start[f->card_page];
    for (int i = 0; i < f->card_page_count[f->card_page]; i++) {
        size_t bytes = strlen(line);
        face_emoji_collect(line, bytes, refs, &count);
        line += bytes + 1;
    }
    for (unsigned i = 0; i < count; i++) {
        if (refs[i].bytes > EMOJI_CLUSTER_MAX_BYTES || !emoji_sequence_valid(refs[i].text, refs[i].bytes)) continue;
        memcpy(keys[usable], refs[i].text, refs[i].bytes);
        keys[usable++][refs[i].bytes] = 0;
    }
    return usable;
}

void face_emoji_reset(face_t *f) {
    face_emoji_t *e = &f->card_emoji;
    // Keep the monotonic request identity: old connection replies cannot match.
    uint32_t id = e->id;
    render_emoji_snapshot_release(&e->glyphs);
    memset(e, 0, sizeof *e);
    e->id = id;
}

void face_emoji_sync(face_t *f) {
    face_emoji_t *e = &f->card_emoji;
    char keys[4][65] = {{0}};
    unsigned count = page_keys(f, keys);
    if (e->valid && e->page == f->card_page && e->count == count &&
        !memcmp(e->glyphs.keys, keys, sizeof keys)) return;
    face_emoji_reset(f);
    e->valid = true;
    e->page = f->card_page;
    e->count = count;
    memcpy(e->glyphs.keys, keys, sizeof keys);
    if (++e->id == 0) ++e->id;
}

static bool card_visible(const face_t *f) {
    if (!f->card_n || f->card_left == 0 || f->dark || f->menu.k >= MENU_SWAP) return false;
    return f->mode != MODE_SETUP || (!f->pair_code[0] && !f->setup_text[0][0]);
}

bool face_emoji_poll(face_t *f, uint32_t *id, char keys[4][65], unsigned *count) {
    face_emoji_sync(f);
    face_emoji_t *e = &f->card_emoji;
    if (!card_visible(f) || !e->count || e->requested) return false;
    e->requested = true;
    e->elapsed = 0;
    *id = e->id;
    *count = e->count;
    memcpy(keys, e->glyphs.keys, sizeof e->glyphs.keys);
    return true;
}

bool face_emoji_receive(face_t *f, uint32_t id, const char *seq, const uint8_t *bytes) {
    face_emoji_sync(f);
    face_emoji_t *e = &f->card_emoji;
    if (!seq || !e->requested || e->id != id) return false;
    size_t n = strnlen(seq, EMOJI_CLUSTER_MAX_BYTES + 1);
    if (!n || n > EMOJI_CLUSTER_MAX_BYTES || !emoji_sequence_valid(seq, n)) return false;
    for (unsigned i = 0; i < e->count; i++) {
        if (strcmp(seq, e->glyphs.keys[i])) continue;
        if (e->glyphs.ready[i]) return false;
        if (bytes) e->glyphs.entries[i]=render_emoji_entry_create(bytes);
        e->glyphs.ready[i] = e->glyphs.entries[i] ? 1 : 2;
        return true;
    }
    return false;
}

void face_emoji_update(face_t *f, float dt) {
    face_emoji_sync(f);
    face_emoji_t *e = &f->card_emoji;
    if (!e->requested || e->elapsed >= 3.f) return;
    e->elapsed += dt;
    if (e->elapsed < 3.f) return;
    for (unsigned i = 0; i < e->count; i++)
        if (!e->glyphs.ready[i]) e->glyphs.ready[i] = 2;
}

void face_emoji_stats(const face_t *f, face_emoji_stats_t *stats) {
    const face_emoji_t *e = &f->card_emoji;
    *stats = (face_emoji_stats_t){.id = e->id, .count = e->count,
        .page = (unsigned)f->card_page, .pages = (unsigned)f->card_pages};
    for (unsigned i = 0; i < e->count; i++) {
        stats->ready += e->glyphs.ready[i] == 1;
        stats->missing += e->glyphs.ready[i] == 2;
    }
}
