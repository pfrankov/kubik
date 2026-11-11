#include "render_emoji.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#endif

struct render_emoji_entry {
    atomic_uint refs;
    uint32_t hash;
    uint8_t pixels[R_EMOJI_BYTES];
};
static atomic_uint live_entries;
static bool reserve_entry(void) {
    unsigned count=atomic_load(&live_entries);
    for(unsigned attempt=0;attempt<2*R_EMOJI_SLOTS;attempt++) {
        if(count>=2*R_EMOJI_SLOTS)return false;
        if(atomic_compare_exchange_strong(&live_entries,&count,count+1))return true;
    }
    return false;
}

render_emoji_entry_t *render_emoji_entry_create(const uint8_t pixels[R_EMOJI_BYTES]) {
    if (!pixels) return NULL;
    if (!reserve_entry()) return NULL;
#ifdef ESP_PLATFORM
#if CONFIG_IDF_TARGET_ESP32S3
    const unsigned caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
#else
    const unsigned caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT|MALLOC_CAP_DMA;
#endif
    render_emoji_entry_t *entry=heap_caps_malloc(sizeof *entry,caps);
#else
    render_emoji_entry_t *entry=malloc(sizeof *entry);
#endif
    if (!entry) { atomic_fetch_sub(&live_entries,1);return NULL; }
    atomic_init(&entry->refs,1);memcpy(entry->pixels,pixels,R_EMOJI_BYTES);
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<R_EMOJI_BYTES;i++)hash=(hash^pixels[i])*16777619u;
    entry->hash=hash;return entry;
}
void render_emoji_entry_retain(render_emoji_entry_t *entry) {
    if(entry)atomic_fetch_add(&entry->refs,1);
}
void render_emoji_entry_release(render_emoji_entry_t *entry) {
    if(!entry || atomic_fetch_sub(&entry->refs,1)!=1)return;
#ifdef ESP_PLATFORM
    heap_caps_free(entry);
#else
    free(entry);
#endif
    atomic_fetch_sub(&live_entries,1);
}
const uint8_t *render_emoji_entry_pixels(const render_emoji_entry_t *entry) {return entry ? entry->pixels:NULL;}
uint32_t render_emoji_entry_hash(const render_emoji_entry_t *entry) {return entry ? entry->hash:0;}
unsigned render_emoji_entries_live(void) {return atomic_load(&live_entries);}
void render_emoji_snapshot_release(render_emoji_snapshot_t *snapshot) {
    for(unsigned i=0;i<R_EMOJI_SLOTS;i++) {
        render_emoji_entry_release(snapshot->entries[i]);snapshot->entries[i]=NULL;
    }
}
