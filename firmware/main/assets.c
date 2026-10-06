#include "assets.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "sprite.h"

static const char *TAG = "assets";

typedef struct {
    char name[24];
    uint32_t off, len;
} entry_t;

static const esp_partition_t *s_part;
static const entry_t *s_dir;
static uint32_t s_count;
static const uint8_t *s_audio;  // mapping of all sfx/ + vo/ entries
static uint32_t s_audio_off;
static uint32_t s_sprites_off, s_sprites_len;

// Sprite mappings: slot 0 holds the pack tables for good, slots 1-2 hold whole
// animations (least recently used goes).
typedef struct {
    uint32_t off, len;
    const uint8_t *ptr;
    esp_partition_mmap_handle_t h;
    uint32_t used;
} slot_t;
static slot_t s_slot[3];
static uint32_t s_clock;

static const uint8_t *map(uint32_t off, uint32_t len, esp_partition_mmap_handle_t *h) {
    const void *p = NULL;
    esp_err_t err = esp_partition_mmap(s_part, off, len, ESP_PARTITION_MMAP_DATA, &p, h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap %lu+%lu: %s", (unsigned long)off, (unsigned long)len, esp_err_to_name(err));
        return NULL;
    }
    return p;
}

static const uint8_t *sprite_fetch(uint32_t off, uint32_t len, uint32_t ho, uint32_t hl) {
    uint32_t a = s_sprites_off + off;
    if (off + len > s_sprites_len) return NULL;
    for (int i = 0; i < 3; i++) {
        slot_t *s = &s_slot[i];
        if (s->ptr && a >= s->off && a + len <= s->off + s->len) {
            s->used = ++s_clock;
            return s->ptr + (a - s->off);
        }
    }
    int k;
    if (ho == 0) {
        k = 0;  // pack tables
    } else {
        k = s_slot[1].used <= s_slot[2].used ? 1 : 2;
    }
    slot_t *s = &s_slot[k];
    if (s->ptr) {
        esp_partition_munmap(s->h);
        s->ptr = NULL;
    }
    if (hl < len || ho > off || ho + hl < off + len) {
        ho = off;
        hl = len;
    }
    s->off = s_sprites_off + ho;
    s->len = hl;
    s->ptr = map(s->off, s->len, &s->h);
    s->used = ++s_clock;
    return s->ptr ? s->ptr + (a - s->off) : NULL;
}

bool assets_init(void) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "assets");
    if (!s_part) {
        ESP_LOGE(TAG, "no assets partition");
        return false;
    }
    esp_partition_mmap_handle_t h;
    const uint8_t *hdr = map(0, 8, &h);
    if (!hdr || memcmp(hdr, "KAST", 4) != 0) {
        ESP_LOGE(TAG, "assets partition is empty: run idf.py flash (it writes build/assets.bin)");
        return false;
    }
    uint32_t n;
    memcpy(&n, hdr + 4, 4);
    esp_partition_munmap(h);
    if (n == 0 || n > 1024) return false;
    const uint8_t *d = map(0, 8 + 32 * n, &h);
    if (!d) return false;
    s_dir = (const entry_t *)(d + 8);
    s_count = n;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t i = 0; i < n; i++) {
        const entry_t *e = &s_dir[i];
        if (strcmp(e->name, "sprites") == 0) {
            s_sprites_off = e->off;
            s_sprites_len = e->len;
        } else {
            if (e->off < lo) lo = e->off;
            if (e->off + e->len > hi) hi = e->off + e->len;
        }
    }
    if (hi > lo) {
        esp_partition_mmap_handle_t ha;
        s_audio = map(lo, hi - lo, &ha);
        s_audio_off = lo;
    }
    bool spr = s_sprites_len != 0;
    ESP_LOGI(TAG, "%lu entries, audio %lu KB, sprites %lu KB %s", (unsigned long)n, (unsigned long)((hi - lo) >> 10),
             (unsigned long)(s_sprites_len >> 10), spr ? "ok" : "MISSING");
    return true;
}

void assets_sprites(bool enabled) {
    if (enabled) {
        if (!sprite_ready() && s_sprites_len) sprite_init(sprite_fetch);
    } else {
        sprite_deinit();
        for (int i = 0; i < 3; i++) {
            if (s_slot[i].ptr) esp_partition_munmap(s_slot[i].h);
            memset(&s_slot[i], 0, sizeof s_slot[i]);
        }
    }
}

const uint8_t *assets_data(const char *name, uint32_t *bytes) {
    for (uint32_t i = 0; s_audio && i < s_count; i++) {
        const entry_t *e = &s_dir[i];
        if (strncmp(e->name, name, sizeof(e->name)) == 0 && e->off >= s_audio_off) {
            if (bytes) *bytes = e->len;
            return s_audio + (e->off - s_audio_off);
        }
    }
    if (bytes) *bytes = 0;
    return NULL;
}

const int16_t *assets_pcm(const char *name, uint32_t *samples) {
    uint32_t bytes = 0;
    const uint8_t *data = assets_data(name, &bytes);
    if (samples) *samples = bytes / 2;
    return (const int16_t *)data;
}
