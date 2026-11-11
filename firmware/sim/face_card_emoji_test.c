#include "../main/face_internal.h"
#include "../main/emoji_cluster.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *emoji[] = {"😀", "👩🏽‍💻", "🇷🇺", "❤️", "🖨️", "👨‍👩‍👧‍👦"};
static face_t face;
static void init(void) {
    face_emoji_reset(&face);memset(&face, 0, sizeof face);
    face.mode = MODE_IDLE;
}
static void limits(void) {
    int line = 0, offset = 0;
    for (int page = 0; page < face.card_pages; page++) {
        face_emoji_key_t keys[4];
        unsigned count = 0;
        assert(face.card_page_start[page] == offset);
        assert(face.card_page_count[page] > 0 && face.card_page_count[page] <= 6);
        for (unsigned j = 0; j < face.card_page_count[page]; j++, line++) {
            const char *text = face_card_line(&face, line);
            offset += (int)strlen(text) + 1;
            assert(rich_label_width(TXT_CARD, text) <= 404);
            assert(face_emoji_collect(text, strlen(text), keys, &count));
        }
        assert(count <= 4);
    }
    assert(line == face.card_n);
}
static size_t payload_bytes(void) {
    size_t bytes = 0;
    for (int i = 0; i < face.card_n; i++) bytes += strlen(face_card_line(&face, i));
    return bytes;
}
static void unique_pages(void) {
    init();
    char text[512] = "";
    for (unsigned i = 0; i < 6; i++) strcat(text, emoji[i]);
    face_card(&face, text);
    assert(face.card_n == 2 && face.card_pages == 2);
    assert(face.card_page_count[0] == 1 && face.card_page_count[1] == 1);
    assert(payload_bytes() == strlen(text));
    limits();
    init();
    text[0] = 0;
    for (unsigned i = 0; i < 6; i++) { strcat(text, emoji[i]); strcat(text, "\n"); }
    face_card(&face, text);
    assert(face.card_n == 6 && face.card_pages == 2);
    assert(face.card_page_count[0] == 4 && face.card_page_count[1] == 2);
    limits();
}
static void long_repeats(void) {
    init();
    char text[1001] = "";
    size_t n = strlen(emoji[1]);
    while (strlen(text) + n <= 1000) strcat(text, emoji[1]);
    face_card(&face, text);
    assert(payload_bytes() == strlen(text));
    limits();
    // Worst-case large ASCII advances consume more than the old forty lines.
    memset(text, 'W', 1000); text[1000] = 0;
    face_card(&face, text);
    assert(face.card_n > 40 && payload_bytes() == 1000);
    limits();
}
static void explicit_short_lines(void) {
    init();
    char text[1001];
    for (int i = 0; i < 500; i++) { text[2*i] = 'x'; text[2*i+1] = '\n'; }
    text[1000] = 0;
    face_card(&face, text);
    assert(face.card_n == 500 && face.card_pages == 84 && payload_bytes() == 500);
    for (int i = 0; i < 500; i++) assert(!strcmp(face_card_line(&face, i), "x"));
    limits();
    memset(text, '\n', 999); text[999] = 'x'; text[1000] = 0;
    face_card(&face, text);
    assert(face.card_n == 1000 && face.card_pages == 167 && payload_bytes() == 1);
    assert(!strcmp(face_card_line(&face, 999), "x"));
    limits();
}
static uint32_t request(unsigned expected) {
    uint32_t id = 0;
    unsigned count = 0;
    char keys[4][65];
    assert(face_emoji_poll(&face, &id, keys, &count));
    assert(id && count == expected);
    assert(!face_emoji_poll(&face, &id, keys, &count));
    return id;
}
static void responses_and_timeout(void) {
    init(); face_card(&face, "😀 😀 👩🏽‍💻 🇷🇺 ❤️");
    uint32_t id = request(4);
    static uint8_t glyph[576]; memset(glyph, 0xA5, sizeof glyph);
    assert(!face_emoji_receive(&face, id + 1, emoji[0], glyph));
    assert(!face_emoji_receive(&face, id, emoji[4], glyph));
    assert(face_emoji_receive(&face, id, emoji[0], glyph));
    assert(!face_emoji_receive(&face, id, emoji[0], glyph));
    assert(!memcmp(render_emoji_entry_pixels(face.card_emoji.glyphs.entries[0]), glyph, sizeof glyph));
    assert(face_emoji_receive(&face, id, emoji[1], NULL));
    face_emoji_update(&face, 2.99f);
    assert(face.card_emoji.glyphs.ready[2] == 0);
    face_emoji_update(&face, .02f);
    assert(!face_emoji_receive(&face, id, emoji[2], glyph));
    face_emoji_stats_t stats; face_emoji_stats(&face, &stats);
    assert(stats.count == 4 && stats.ready == 1 && stats.missing == 3);
    face_emoji_reset(&face);
    assert(request(4) != id);
}
static void visibility_and_pages(void) {
    init(); face_card(&face, "😀\n👩🏽‍💻\n🇷🇺\n❤️\n🖨️");
    uint32_t id; unsigned count; char keys[4][65];
    face.dark = true;
    assert(!face_emoji_poll(&face, &id, keys, &count));
    face.dark = false; face.menu.k = MENU_SWAP;
    assert(!face_emoji_poll(&face, &id, keys, &count));
    face.menu.k = 0; face.mode = MODE_SETUP; strcpy(face.pair_code, "1234");
    assert(!face_emoji_poll(&face, &id, keys, &count));
    face.mode = MODE_IDLE;
    id = request(4);
    assert(face_card_tap(&face) && face.card_page == 1);
    uint32_t next = request(1); assert(next != id);
    assert(!face_emoji_receive(&face, id, emoji[0], NULL));
    assert(face_emoji_receive(&face, next, emoji[4], NULL));
    // A streaming extension keeps the reader's page; automatic paging remains.
    face_card(&face, "😀\n👩🏽‍💻\n🇷🇺\n❤️\n🖨️\nmore");
    assert(face.card_page == 1);
    assert(face_card_tap(&face) && face.card_left > 0);
    card_update(&face, .36f); assert(!face.card_n && !face.card_pages);
    face_card(&face, "a\nb\nc\nd\ne\nf\ng");
    card_update(&face, 4.01f); assert(face.card_page == 1);
}
static void scene_copy(void) {
    init(); face_card(&face, "😀 👩🏽‍💻");
    uint32_t id = request(2);
    uint8_t glyph[576]; memset(glyph, 0x56, sizeof glyph);
    assert(face_emoji_receive(&face, id, emoji[0], glyph));
    static scene_t scene; scene_begin(&scene, 0); face.card_t = 1;
    draw_card(&face, &scene);
    assert(scene.text_n == 3 && scene.text[0] == R_EMOJI_TOKEN && scene.text[2] == R_EMOJI_TOKEN + 1);
    assert(scene.emoji.ready[0] == 1);
    assert(!memcmp(render_emoji_entry_pixels(scene.emoji.entries[0]), glyph, sizeof glyph));
    face_emoji_reset(&face);
    assert(!memcmp(render_emoji_entry_pixels(scene.emoji.entries[0]), glyph, sizeof glyph));scene_end(&scene);
}
static void oversized_cluster(void) {
    init();
    char text[300] = "😀";
    for (int i = 0; i < 20; i++) strcat(text, "‍😀");
    assert(strlen(text) > 64);
    face_card(&face, text);
    assert(face.card_n == 1 && payload_bytes() == strlen(text));
    face_emoji_sync(&face); assert(face.card_emoji.count == 0);
    face.card_t = 1;
    static scene_t scene; scene_begin(&scene, 0); draw_card(&face, &scene);
    assert(scene.text_n == 1 && scene.text[0] == R_EMOJI_PLACEHOLDER);scene_end(&scene);
}
static void held_frame_and_new_page(void) {
    init();face_card(&face,"😀 👩🏽‍💻 🇷🇺 ❤️");uint32_t id=request(4);
    uint8_t old[576],next[576];memset(old,0x56,sizeof old);memset(next,0xa5,sizeof next);
    for(unsigned i=0;i<4;i++)assert(face_emoji_receive(&face,id,emoji[i],old));
    scene_t held={0};scene_begin(&held,0);face.card_t=1;draw_card(&face,&held);
    face_card(&face,"🖨️ 👨‍👩‍👧‍👦 👍🏽 🎉");id=request(4);
    const char *keys[]={"🖨️","👨‍👩‍👧‍👦","👍🏽","🎉"};
    for(unsigned i=0;i<4;i++)assert(face_emoji_receive(&face,id,keys[i],next));
    assert(render_emoji_entries_live()==8 && !render_emoji_entry_create(next));
    assert(!memcmp(render_emoji_entry_pixels(held.emoji.entries[0]),old,sizeof old));
    scene_end(&held);assert(render_emoji_entries_live()==4);face_emoji_reset(&face);
    assert(!render_emoji_entries_live());
}
static void full_store_is_terminal(void) {
    init();uint8_t pixels[576]={0};render_emoji_entry_t *held[8];
    for(unsigned i=0;i<8;i++){held[i]=render_emoji_entry_create(pixels);assert(held[i]);}
    face_card(&face,"😀");uint32_t id=request(1);assert(face_emoji_receive(&face,id,"😀",pixels));
    assert(face.card_emoji.glyphs.ready[0]==2 && !face.card_emoji.glyphs.entries[0]);
    assert(!face_emoji_receive(&face,id,"😀",pixels));
    uint32_t ignored;unsigned count;char keys[4][65];assert(!face_emoji_poll(&face,&ignored,keys,&count));
    for(unsigned i=0;i<8;i++)render_emoji_entry_release(held[i]);face_emoji_reset(&face);
    assert(!render_emoji_entries_live());
}
int main(void) {
    unique_pages(); long_repeats(); explicit_short_lines(); responses_and_timeout();
    visibility_and_pages(); scene_copy(); oversized_cluster();
    held_frame_and_new_page();full_store_is_terminal();face_emoji_reset(&face);assert(!render_emoji_entries_live());
    printf("Card emoji: whole clusters, 1000 bytes, <=4 unique/6 lines, terminal timeout, paging and immutable scene; face cache %zu bytes\n", sizeof(face_emoji_t));
}
