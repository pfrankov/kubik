// Native portrait UI: inspect the real scene and route touches at visible controls.
#include "../main/face_internal.h"
#include <assert.h>

static const prim_t *word(const scene_t *s, const char *text) {
    for (int i = 0; i < s->n; i++) {
        const prim_t *p = &s->p[i];
        if (p->kind != PK_TEXT || p->b != (int)strlen(text)) continue;
        int j = 0;
        while (j < p->b && s->text[p->a + j] == font_glyph(&g_fonts[p->r], text[j])) j++;
        if (j == p->b) return p;
    }
    return NULL;
}
static void bounds(const scene_t *s) {
    assert(s->n < R_MAX_PRIMS);
    for (int i = 0; i < s->n; i++) {
        const prim_t *p = &s->p[i];
        if (p->kind != PK_TEXT && p->kind != PK_QR && p->kind != PK_ICON) continue;
        assert(p->bx0 >= 0 && p->bx1 < UI_W);
        assert(p->by0 >= 0 && p->by1 < UI_H);
    }
}
static void tap_menu_word(face_t *f, const scene_t *s, const char *text, int expected) {
    const prim_t *p = word(s, text); assert(p);
    int part;
    assert(face_menu_hit(f, (p->bx0 + p->bx1) / 2, (p->by0 + p->by1) / 2, &part) == expected);
}
static void menu(void) {
    face_t f; face_init(&f); f.menu.open = true; f.menu.k = 1;
    f.menu.volume = 50; f.menu.brightness = 50; f.menu.txt[MT_POWER] = "Power off";
    scene_t s; face_draw(&f, &s); bounds(&s);
    tap_menu_word(&f, &s, "Agent", MENU_AGENT);
    tap_menu_word(&f, &s, "Guide", MENU_GUIDE);
    tap_menu_word(&f, &s, "Status", MENU_STATUS);
    tap_menu_word(&f, &s, "Power off", MENU_ACTIONS);
    const prim_t *status = word(&s, "Status");
    assert(status->by0 > 500 && status->by1 < 564);
    int part;
    for (int y = 84; y <= 472; y += 4) {
        assert(face_menu_hit(&f, 72, y, &part) == MENU_VOLUME);
        assert(face_menu_hit(&f, 188, y, &part) == MENU_BRIGHT);
        assert(face_menu_hit(&f, 246, y, &part) == -1);
    }
    assert(face_menu_slider(84) == 100 && face_menu_slider(472) == 0);
    assert(face_menu_hit(&f, 356, 278, &part) == -1); // equal navigation gutter
    f.menu.sound = true; face_draw(&f, &s); bounds(&s);
    assert(word(&s, "Speech")->by0 > 500 && word(&s, "Interface")->by0 > 500);
    assert(face_menu_hit(&f, 132, 470, &part) == MENU_VOLUME);
    assert(face_menu_hit(&f, 348, 470, &part) == MENU_UI_VOLUME);
}
static void status(void) {
    face_t f; face_init(&f); f.menu.open = true; f.menu.k = 1;
    f.status.open = f.status.radio = f.status.connected = f.status.agent = true;
    f.status.enter = 1;
    strcpy(f.status.ssid, "Native portrait network"); strcpy(f.status.url, "kubik://agent.example:18793");
    const char *pages[] = {"Network", "Device", "Agent"};
    for (unsigned i = 0; i < 3; i++) {
        f.status.page = i;
        scene_t s; face_draw(&f, &s); bounds(&s);
        const prim_t *p = word(&s, pages[(i + 1) % 3]); assert(p && p->by0 > 500);
        device_status_tap(&f.status, (p->bx0 + p->bx1) / 2, (p->by0 + p->by1) / 2);
        assert(f.status.page == (i + 1) % 3);
    }
    f.status.page = 0;
    scene_t s; face_draw(&f, &s);
    const prim_t *wifi = word(&s, "Wi-Fi"); assert(wifi);
    device_status_tap(&f.status, (wifi->bx0 + wifi->bx1) / 2, (wifi->by0 + wifi->by1) / 2);
    assert(f.status.wifi_setup);
}
static void setup(void) {
    static const int sizes[] = {21, 25, 29, 33, 37, 41};
    static const int modules[] = {8, 6, 6, 5, 4, 4}; // prior maximum camera target retained
    static uint8_t qr[41 * 41];
    face_t f; face_init(&f); face_set_mode(&f, MODE_SETUP);
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        face_set_setup(&f, qr, sizes[i], 1, "1 - SCAN TO JOIN WI-FI", "192.168.4.1"); f.qr_t = 1;
        scene_t s; face_draw(&f, &s); bounds(&s);
        unsigned count = 0;
        for (int n = 0; n < s.n; n++) if (s.p[n].kind == PK_QR) {
            const prim_t *p = &s.p[n]; count++;
            assert(p->b == modules[i]);
            assert(abs((p->by0 + p->by1) / 2 - 292) <= 1);
            assert(p->bx1 - p->bx0 == p->by1 - p->by0);
        }
        assert(count == 1 && word(&s, "192.168.4.1")->by0 > 520);
    }
    face_set_setup(&f, NULL, 0, 0, NULL, NULL); face_set_pairing(&f, "K7QX4M2R"); f.qr_t = 1;
    for (unsigned telegram = 0; telegram < 2; telegram++) for (unsigned hermes = 0; hermes < 2; hermes++) {
        f.pair_telegram = telegram; f.pair_hermes = hermes;
        scene_t s; face_draw(&f, &s); bounds(&s);
        const prim_t *heading = word(&s, "PAIR WITH AGENT"); assert(heading && heading->by0 > 160);
    }
}
static void cards(void) {
    face_t f; face_init(&f); face_set_character(&f, CHARACTER_TESS); face_set_mode(&f, MODE_IDLE);
    face_card(&f, "First line\nSecond line\nThird line\nFourth line\nFifth line\nSixth line\nSeventh line");
    for (unsigned live = 0; live < 2; live++) for (unsigned frame = 1; frame <= 25; frame++) {
        f.live_active = live; f.card_t = frame / 60.f; f.card_hold = true;
        scene_t s; face_draw(&f, &s); bounds(&s);
        if (live) assert(word(&s, "BOOT to end") && word(&s, "Connecting"));
        unsigned lines = 0;
        for (int i = 0; i < s.n; i++) if (s.p[i].kind == PK_TEXT && s.p[i].r == FONT_CARD) {
            lines++;
            if (live) assert(s.p[i].by1 < 493); // room for the physical BOOT/Live footer
        }
        assert(lines == 6);
    }
}
static void events(void) {
    event_journal_t j = {0};
    for (unsigned i = 0; i < 6; i++) {
        char text[32]; snprintf(text, sizeof text, "Event %u", i);
        event_journal_add(&j, i, JOURNAL_REPLY, text, "Text card");
    }
    event_journal_open(&j); j.enter = 1;
    for (int y = 128; y < 532; y++) {
        j.detail = false; event_journal_tap(&j, 240, y);
        assert(j.detail && j.selected == (y < 262 ? 0 : y < 397 ? 1 : 2));
    }
    event_journal_back(&j); j.enter = 1;
    scene_t s; scene_begin(&s, 0); event_journal_draw(&j, &s); bounds(&s);
    const prim_t *last = word(&s, "Event 3"); assert(last && last->by0 > 420);
    event_journal_tap(&j, (last->bx0 + last->bx1) / 2, (last->by0 + last->by1) / 2);
    assert(j.detail && j.selected == 2);
    event_journal_back(&j); event_journal_tap(&j, 392, 554); assert(j.offset == 3);
    scene_begin(&s, 0); event_journal_draw(&j, &s); bounds(&s);
    const prim_t *first = word(&s, "Event 2"); assert(first);
    event_journal_tap(&j, (first->bx0 + first->bx1) / 2, (first->by0 + first->by1) / 2);
    scene_begin(&s, 0); event_journal_draw(&j, &s); bounds(&s);
}
int main(void) {
    _Static_assert(UI_W == 480 && UI_H == 584 && R_H == 296, "native S3 portrait viewport");
    menu(); status(); setup(); cards(); events();
    puts("S3 portrait: real scenes fit 480x584; lower controls, QR proportions, six-line cards and hit routing passed");
}
