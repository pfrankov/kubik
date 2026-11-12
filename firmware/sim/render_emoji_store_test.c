#include "render_emoji.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
static bool fail_allocation;
static unsigned allocations,frees,attempts;
void *heap_caps_malloc(size_t bytes,unsigned caps) {
    unsigned expected=CONFIG_IDF_TARGET_ESP32S3 ? MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT :
        MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT|MALLOC_CAP_DMA;
    assert(bytes==584 && caps==expected);attempts++;
    if(fail_allocation) { return NULL; }
    allocations++;return malloc(bytes);
}
void heap_caps_free(void *data) {assert(data);frees++;free(data);}
int main(void) {
    uint8_t pixels[R_EMOJI_BYTES];memset(pixels,0xa5,sizeof pixels);
    assert(!render_emoji_entries_live());fail_allocation=true;
    assert(!render_emoji_entry_create(pixels) && !render_emoji_entries_live());fail_allocation=false;
    render_emoji_entry_t *entries[8];
    for(unsigned i=0;i<8;i++){entries[i]=render_emoji_entry_create(pixels);assert(entries[i]);}
    unsigned before=attempts;assert(!render_emoji_entry_create(pixels) && attempts==before);
    render_emoji_entry_retain(entries[0]);render_emoji_entry_release(entries[0]);
    memset(pixels,0,sizeof pixels);assert(render_emoji_entry_pixels(entries[0])[0]==0xa5);
    assert(render_emoji_entries_live()==8);
    for(unsigned i=0;i<8;i++)render_emoji_entry_release(entries[i]);
    assert(!render_emoji_entries_live() && allocations==frees);
    render_emoji_entry_t *again=render_emoji_entry_create(pixels);assert(again);render_emoji_entry_release(again);
    assert(!render_emoji_entries_live() && allocations==frees);
    printf("emoji store: strict %s 584-byte entries, immutable pins, cap8, allocation failure and release-to-zero passed\n",
        CONFIG_IDF_TARGET_ESP32S3 ? "PSRAM" : "internal DMA");
}
