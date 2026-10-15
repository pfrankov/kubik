// Test with the native 80-slot reference: saturation at a reduced device cap cannot hide dropped overlays.
#include "../main/face.h"
#include "../main/face_internal.h"
#include "../main/sprite.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t *pack;
static size_t pack_size;
static unsigned maximum, cases;
static const uint8_t *fetch(uint32_t offset, uint32_t bytes, uint32_t hint, uint32_t length) {
    (void)hint; (void)length;
    return offset <= pack_size && bytes <= pack_size - offset ? pack + offset : NULL;
}
static void card(face_t *f) {
    f->card_n = FACE_CARD_LINES; f->card_t = 1; f->card_left = -1; f->card_hold = true;
    for (unsigned i = 0; i < FACE_CARD_LINES; ++i) {
        f->card_line[i] = i * 20;
        memcpy(f->card_buf + i * 20, "A full text line", sizeof "A full text line");
    }
}
static void check(face_t *f) {
    scene_t scene; face_draw(f, &scene);
    assert(scene.n < 64); // The device keeps all primitives plus spare capacity.
    assert(scene.text_n < R_TEXT_POOL && scene.dots_n <= R_MAX_DOTS);
    if ((unsigned)scene.n > maximum) maximum = scene.n;
    ++cases;
}
static void overlays(face_t *f) {
    face_set_power(f, true, 15, true, true);
    f->cron_running = 2; f->cron_due = 3; f->cron_k = 1;
    f->offline_icon = f->offline_icon_last = BUB_NO_SERVER; f->offline_k = 1;
    f->live_active = f->live_ready = f->live_mic = true;
    card(f);
    f->bub_t = 1; f->bub_left = 10; strcpy(f->bub_text, "Status message");
    for (int page = 0; page < (FACE_CARD_LINES + FACE_CARD_PAGE - 1) / FACE_CARD_PAGE; ++page) {
        f->card_page = page;
        for (int icon = 0; icon < BUB_COUNT; ++icon) {
            f->bub_icon = icon;
            f->menu.k = 0; check(f);
            f->menu.k = MENU_SWAP * .5f; check(f); // Fade and home overlays coexist.
        }
    }
    f->menu.k = 0;
    f->card_n = 0; f->journal.overlay = true;
    event_journal_add(&f->journal, 1, JOURNAL_ACTIVITY, "A visible event", "Listening");
    check(f);
}
int main(void) {
    assert(R_MAX_PRIMS == 80); // Native reference remains larger than the firmware limit.
    FILE *fp = fopen("firmware/assets/sprites.bin", "rb"); assert(fp);
    fseek(fp, 0, SEEK_END); pack_size = ftell(fp); rewind(fp);
    pack = malloc(pack_size); assert(pack && fread(pack, 1, pack_size, fp) == pack_size); fclose(fp);
    assert(sprite_init(fetch));
    for (int mode = MODE_BOOT; mode <= MODE_OFFLINE; ++mode) {
        for (int emotion = 0; emotion < EMO_COUNT; ++emotion) {
            face_t f; face_init(&f); face_set_character(&f, CHARACTER_TESS);
            f.boot_t = 5; face_set_mode(&f, mode); face_set_emotion(&f, emotion, -1);
            for (int tick = 0; tick < 24; ++tick) {
                f.mic_level = (tick % 4) / 3.f; f.spk_level = ((tick + 2) % 4) / 3.f;
                face_update(&f, 1.f / R_FPS); overlays(&f);
            }
        }
    }
    printf("Tess scene capacity: %u combinations, max %u/64; full cards/pages, Live, charge, cron, bubble, fade and event overlay\n", cases, maximum);
    free(pack);
}
