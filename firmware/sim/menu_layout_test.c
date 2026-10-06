// Actual scene geometry and touch routing, independent of the layout's constants.
#include "../main/face.h"
#include "../main/ui_text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const prim_t *text(const scene_t *s, const char *word) {
    for (int i = 0; i < s->n; i++) {
        const prim_t *p = &s->p[i];
        if (p->kind != PK_TEXT || p->b != (int)strlen(word)) continue;
        bool same = true;
        for (int j = 0; j < p->b; j++)
            if (s->text[p->a + j] != font_glyph(&g_fonts[p->r], word[j])) same = false;
        if (same) return p;
    }
    return NULL;
}
static const prim_t *icon(const scene_t *s, int id) {
    for (int i = 0; i < s->n; i++) if (s->p[i].kind == PK_ICON && s->p[i].b == id) return &s->p[i];
    return NULL;
}
static void navigation(const face_t *face, const scene_t *scene, const char *word, int id, int row) {
    const prim_t *label = text(scene, word), *glyph = icon(scene, id);
    assert(label && glyph && glyph->bx1 + 8 < label->bx0);
    assert(glyph->bx1 - glyph->bx0 >= 30); // strokes survive the native screen scale
    int x = (label->bx0 + glyph->bx1) / 2, y = (glyph->by0 + glyph->by1) / 2, part;
    // A finger-sized rectangle around the visible label/icon routes to the same action.
    for (int dy = -40; dy <= 40; dy += 8) for (int dx = -60; dx <= 60; dx += 8)
        assert(face_menu_hit(face, x + dx, y + dy, &part) == row);
    assert(label->by0 >= 68 && label->by1 <= 384 && label->bx1 <= 440);
}
static void menu_geometry(void) {
    face_t face; face_init(&face);
    face.menu.open = true; face.menu.k = 1;
    face.menu.volume = 70; face.menu.brightness = 80;
    face.menu.txt[MT_WIFI] = "Wi-Fi"; face.menu.txt[MT_POWER] = "Power off"; face.menu.txt[MT_RESET] = "Reset";
    scene_t scene; face_draw(&face, &scene);
    assert(scene.n < R_MAX_PRIMS && scene.dots_n == 0);
    assert(text(&scene, "Settings") && !text(&scene, "Events"));
    int header_part;
    assert(face_menu_hit(&face, 272, 36, &header_part) == -1);
    face.menu.service = true; face_draw(&face, &scene);
    assert(text(&scene, "Events") && face_menu_hit(&face, 272, 36, &header_part) == MENU_EVENTS);
    face.menu.service = false; face_draw(&face, &scene);
    assert(!text(&scene, "Tess") && !text(&scene, "Plush"));
    navigation(&face, &scene, "Agent", ICON_BRAIN, MENU_AGENT);
    navigation(&face, &scene, "Guide", ICON_CIRCLE_HELP, MENU_GUIDE);
    navigation(&face, &scene, "Status", ICON_GLOBE, MENU_STATUS);
    int part;
    for (int tick = 0; tick < 50; tick++) {
        face.menu.k = tick / 50.f;
        assert(face_menu_hit(&face, 356, 116, &part) == -1);
        assert(face_menu_hit(&face, 356, 226, &part) == -1);
        assert(face_menu_hit(&face, 72, 226, &part) == -1);
    }
    face.menu.k = 1;
    for (int y = 76; y <= 376; y += 16) {
        assert(face_menu_hit(&face, 72, y, &part) == MENU_VOLUME);
        assert(face_menu_hit(&face, 188, y, &part) == MENU_BRIGHT);
        assert(face_menu_hit(&face, 130, y, &part) == -1);
        assert(face_menu_hit(&face, 246, y, &part) == -1);
    }
    assert(face_menu_hit(&face, 356, 171, &part) == -1); // navigation gutter
    assert(face_menu_hit(&face, 240, 428, &part) == -1); // no invisible reset/power target
    assert(face_menu_slider(0) == 100 && face_menu_slider(480) == 0);
    int last = 100;
    for (int y = 0; y <= 480; y++) { int value = face_menu_slider(y); assert(value <= last); last = value; }
    face.menu.open = false;
    assert(face_menu_hit(&face, 356, 116, &part) == -1);
}
static void menu_power_indicator(face_t *face) {
    scene_t scene;
    face->menu.open = true; face->menu.k = 1;
    face_set_power(face, true, 100, false, true);
    face_draw(face, &scene);
    assert(icon(&scene, ICON_ZAP) && text(&scene, "100%"));
    face_set_power(face, true, 60, true, true);
    face_draw(face, &scene); assert(icon(&scene, ICON_ZAP) && text(&scene, "60%"));
    face_set_power(face, false, -1, false, true);
    face_draw(face, &scene); assert(icon(&scene, ICON_ZAP));
    face_set_power(face, true, 60, false, false);
    face_draw(face, &scene); assert(!icon(&scene, ICON_ZAP) && text(&scene, "60%"));
    face->menu.open = false; face->menu.k = 0;
}
static void power_indicator(void) {
    for (int character = 0; character < CHARACTER_COUNT; character++) {
        face_t face; face_init(&face); face_set_character(&face, character);
        face_set_mode(&face, MODE_IDLE);
        scene_t scene;
        face_set_power(&face, true, 100, false, true); // full battery, cable still connected
        face_draw(&face, &scene);
        assert(!icon(&scene, ICON_ZAP) && !text(&scene, "100%"));
        face_set_power(&face, true, 60, true, true);
        for (int word = 0; word < STR_COUNT; word++) {
            face_bubble(&face, BUB_BUSY, str(word), 1); face.bub_t = 1;
            face_draw(&face, &scene);
            const prim_t *bolt = icon(&scene, ICON_ZAP);
            assert(bolt && bolt->bx0 >= 24 && bolt->by0 >= 24);
            for (int i = 0; i < scene.n; i++) {
                const prim_t *p = &scene.p[i];
                if (p->kind == PK_TEXT) assert(p->bx0 > bolt->bx1 || p->by0 > bolt->by1);
            }
        }
        face.bub_icon = BUB_NONE;
        face_set_power(&face, true, 60, true, true);
        face_draw(&face, &scene);
        assert(icon(&scene, ICON_ZAP) && !text(&scene, "60%"));
        face_set_power(&face, true, 60, false, false);
        face_draw(&face, &scene);
        assert(!icon(&scene, ICON_ZAP) && !text(&scene, "60%"));
        face_set_power(&face, true, 15, false, false);
        face_draw(&face, &scene);
        assert(!icon(&scene, ICON_ZAP) && text(&scene, "15%"));
        face_set_power(&face, false, -1, false, true); // external supply without a battery
        face_draw(&face, &scene); assert(!icon(&scene, ICON_ZAP));
        face_set_power(&face, false, -1, false, false); // unknown reading
        face_draw(&face, &scene); assert(!icon(&scene, ICON_ZAP));
        menu_power_indicator(&face);
        face_set_power(&face, true, 15, false, true); // cable alone never claims active charging
        face_draw(&face, &scene); assert(!icon(&scene, ICON_ZAP) && text(&scene, "15%"));
    }
}
static void sound_geometry(void) {
    face_t face = {0}; face.menu.open = true; face.menu.k = 1;
    face.menu.sound = true; face.menu.volume = 70; face.menu.ui_volume = 0;
    face.menu.pressed = -1;
    int part;
    assert(face_menu_hit(&face, 132, 226, &part) == MENU_VOLUME);
    assert(face_menu_hit(&face, 348, 226, &part) == MENU_UI_VOLUME);
    assert(face_menu_hit(&face, 240, 226, &part) == -1);
    assert(face_menu_hit(&face, 127, 428, &part) == -1); // old Wi-Fi cannot fire
    scene_t scene; scene_begin(&scene, 0); face_draw(&face, &scene);
    assert(text(&scene, "Sound") && text(&scene, "Speech") && text(&scene, "Interface"));
    assert(!text(&scene, "Agent") && !text(&scene, "Guide"));
    assert(icon(&scene, ICON_AUDIO_LINES) && icon(&scene, ICON_VOLUME_X));
    assert(text(&scene, "Speech")->color == text(&scene, "Interface")->color);
    face.menu.ui_volume = 70; face_draw(&face, &scene);
    assert(icon(&scene, ICON_VOLUME_2) && icon(&scene, ICON_AUDIO_LINES));
}
static void home_status_alignment(void) {
    face_t f; face_init(&f); face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE); f.cron_k=1; f.cron_due=-1;
    scene_t scene; face_draw(&f,&scene);
    bool clock=false;
    for(int n=0;n<scene.n;n++) if(scene.p[n].kind==PK_RING && scene.p[n].cx==426*16 && scene.p[n].cy==54*16) clock=true;
    assert(clock);
    face_set_power(&f,true,60,true,true);face_draw(&f,&scene);
    assert(icon(&scene,ICON_ZAP)->sub_x==36*16);
    assert(icon(&scene,ICON_ZAP)->sub_y==36*16);
    face_set_power(&f,true,60,false,false);
    int badges[]={BUB_NO_WIFI,BUB_NO_SERVER};
    int glyphs[]={ICON_WIFI_OFF,ICON_UNPLUG};
    for(int n=0;n<2;n++) {
        f.cron_k=0; f.offline_k=1; f.offline_icon=badges[n]; face_draw(&f,&scene);
        const prim_t *p=icon(&scene,glyphs[n]); assert(p);
        assert(p->sub_x==426*16 && p->sub_y==54*16 && 480*16-p->sub_x==p->sub_y);
        face_set_power(&f,true,10,false,false);face_draw(&f,&scene);
        p=icon(&scene,glyphs[n]);assert(text(&scene,"10%")->by0>p->by1);
    }
}
static void status_navigation(void) {
    face_t f; face_init(&f); f.menu.open=true; f.menu.k=1;
    f.status=(device_status_t){.open=true,.connected=true,.radio=true};
    strcpy(f.status.ssid,"Demo Wi-Fi"); strcpy(f.status.ip,"10.1.30.53");
    scene_t scene; face_draw(&f,&scene); assert(text(&scene,"10.1.30.53"));
    assert(device_status_drag(&f.status,300,200,true));
    assert(device_status_drag(&f.status,240,200,false) && f.status.page==1);
    device_status_drag(&f.status,100,200,false); assert(f.status.page==1);
    device_status_tap(&f.status,452); assert(f.status.page==2);
    device_status_drag(&f.status,100,200,true); device_status_drag(&f.status,170,200,false);
    assert(f.status.page==1); device_status_tap(&f.status,200); assert(f.status.page==1);
    f.status.page=2; memset(f.status.url,'W',159); f.status.url[159]=0;
    face_draw(&f,&scene); int letters=0;
    for (int i=0;i<scene.n;i++) {
        const prim_t *p=&scene.p[i];
        if (p->kind==PK_TEXT && p->by0>=220 && p->by1<440) {
            letters+=p->b; assert(p->bx0>=20 && p->bx1<=460);
        }
    }
    assert(letters==159);
    assert(device_status_back(&f.status) && !f.status.open && f.menu.open);
    assert(!device_status_tap(&f.status,452));
}
int main(void) {
    status_navigation(); menu_geometry(); power_indicator(); sound_geometry(); home_status_alignment();
    puts("menu: separated finger targets, icon/label spacing, continuous sliders; charging/full/unplugged/unknown power passed");
}
