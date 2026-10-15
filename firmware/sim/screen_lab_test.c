#include "../main/screen_lab.h"
#include "../main/face_agent.h"
#include "../main/tess.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static screen_lab_t lab;
static scene_t scene, native;
static void tap(int x, int y) { screen_lab_event(&lab, LAB_TAP, x, y); }
static void boot(void) { screen_lab_event(&lab, LAB_BOOT, 0, 0); }
static void assert_native_scene(void) {
    screen_lab_draw(&lab, &scene);
    face_draw(&lab.face, &native);
    assert(scene.n == native.n && scene.dots_n == native.dots_n && scene.text_n == native.text_n);
    assert(!memcmp(scene.p, native.p, scene.n * sizeof scene.p[0]));
    assert(!memcmp(scene.dots, native.dots, scene.dots_n * sizeof scene.dots[0]));
    assert(!memcmp(scene.text, native.text, scene.text_n));
}
static void entry(void) {
    screen_lab_unlock_t state = {0};
    assert(!screen_lab_entry(false, 52, 36));
    for (int i = 0; i < 4; i++) assert(!screen_lab_unlock_tap(&state, 52, 36, i * 200));
    assert(screen_lab_unlock_tap(&state, 52, 36, 800));
    assert(screen_lab_entry(state.ready, 52, 36));
    assert(!screen_lab_entry(state.ready, 400, 36));
    assert(!screen_lab_entry(state.ready, 52, 200));
    screen_lab_unlock_tap(&state, 400, 36, 1000);
    assert(!state.ready); // battery/reset gesture does not unlock the lab
    state = (screen_lab_unlock_t){0};
    for (int i = 0; i < 5; i++) screen_lab_unlock_tap(&state, 52, 36, i * 4100);
    assert(!state.ready);
}
static void catalog(void) {
    assert(LAB_HOME == 0 && LAB_EVENT_LOG == 21 && LAB_GROW_POINT == 22 && LAB_COUNT == 32);
    screen_lab_select(&lab, -1);
    tap(350, 428); assert(lab.page == 1);
    tap(240, 144); assert(lab.selected == LAB_SETUP_QR);
    boot(); assert(lab.selected == -1);
    tap(100, 428); assert(lab.page == 0);
    tap(240, 320); assert(lab.selected == LAB_WIFI_QR);
    screen_lab_event(&lab, LAB_KEY, 0, 0); assert(lab.selected == LAB_SETUP_QR);
    screen_lab_select(&lab, -1); lab.page = 0;
    for (int page = 0; page < 10; page++) tap(350, 428);
    assert(lab.page == 10);
    tap(240, 192); assert(lab.selected == LAB_PLAY_SELF);
}
static void qr(void) {
    for (int panel = LAB_WIFI_QR; panel <= LAB_SETUP_QR; panel++) {
        screen_lab_select(&lab, panel);
        assert(lab.face.qr_n > 0 && lab.face.qr == lab.qr);
        screen_lab_update(&lab, 1.f / 30);
        screen_lab_draw(&lab, &scene); face_draw(&lab.face, &native);
        assert(scene.n == native.n && !memcmp(scene.p, native.p, scene.n * sizeof scene.p[0]));
        // Lab does not add an overlay or steal space from the production QR.
        assert(!screen_lab_controls(&lab));
    }
}
static void settings(void) {
    screen_lab_select(&lab, LAB_SETTINGS);
    tap(72, 100); assert(lab.volume > 80);
    int volume = lab.volume;
    screen_lab_event(&lab, LAB_HOLD, 52, 36); assert(!lab.face.menu.sound);
    screen_lab_event(&lab, LAB_HOLD, 188, 200); assert(!lab.face.menu.sound);
    screen_lab_event(&lab, LAB_HOLD, 72, 200); assert(lab.face.menu.sound);
    tap(348, 340); assert(lab.ui_volume < 20);
    boot(); assert(!lab.face.menu.sound && lab.face.menu.open);
    tap(356, 116); assert(lab.face.agent.open);
    tap(240, 160); assert(lab.face.agent.view == AGENT_VIEW_MODELS);
    tap(240, 195); assert(!strcmp(lab.face.agent.selected_model, "demo/model-1"));
    // A finger drag scrolls the picker and the list coasts after release.
    screen_lab_pointer(&lab, true, 240, 330); screen_lab_pointer(&lab, true, 240, 310);
    for (int y = 290; y >= 190; y -= 20) { screen_lab_pointer(&lab, true, 240, y); screen_lab_update(&lab, 1.f / 30); }
    screen_lab_pointer(&lab, false, 240, 190);
    float released = lab.face.agent.scroll;
    assert(released > 80 && lab.face.agent.scroll_v > 0);
    for (int i = 0; i < 10; i++) screen_lab_update(&lab, 1.f / 30);
    assert(lab.face.agent.scroll > released);
    boot(); assert(lab.face.agent.view == AGENT_VIEW_OVERVIEW);
    boot(); assert(!lab.face.agent.open);
    tap(356, 308); assert(lab.face.agent.view == AGENT_VIEW_GUIDE);
    assert(lab.face.agent.volume == lab.volume);
    tap(240, 428); boot(); assert(lab.face.agent.guide_step == 0 && lab.face.agent.open);
    for (int i = 0; i < 3; i++) { tap(240, 428); assert(lab.face.agent.guide_step == i + 1); }
    tap(240, 428); assert(!lab.face.agent.open);
    boot(); assert(lab.selected == -1);
    screen_lab_select(&lab, LAB_SOUND); assert(lab.face.menu.volume == volume);
}
static void voice(void) {
    screen_lab_select(&lab, LAB_VOICE);
    tap(240, 428); // Voice modes have three choices; below them is Text only.
    assert(lab.face.agent.open);
    screen_lab_select(&lab, LAB_VOICE);
    screen_lab_pointer(&lab, true, 240, 300);
    screen_lab_pointer(&lab, true, 240, 330);
    screen_lab_pointer(&lab, false, 240, 330);
    tap(240, 300); assert(lab.face.agent.view == AGENT_VIEW_VOICE_MODES);
    tap(240, 300);
    assert(lab.face.agent.voice_mode == VOICE_REALTIME);
    assert(lab.face.agent.view == AGENT_VIEW_MODELS && lab.face.agent.target == AGENT_TARGET_VOICE);
    boot(); assert(lab.face.agent.view == AGENT_VIEW_VOICE_MODES);
    tap(240, 120); assert(lab.face.agent.view == AGENT_VIEW_CLASSIC);
    tap(240, 200); assert(lab.face.agent.target == AGENT_TARGET_STT);
    tap(240, 195); boot(); assert(!strcmp(lab.face.agent.stt_model, "Detailed"));
}
static void motion(void) {
    screen_lab_select(&lab, LAB_HOME);
    for (int i = 0; i < 60; i++) screen_lab_update(&lab, 1.f / 30);
    screen_lab_pointer(&lab, true, 240, 240);
    for (int i = 0; i < 6; i++) { screen_lab_pointer(&lab, true, 240, 240 + i * 20); screen_lab_update(&lab, 1.f / 30); }
    screen_lab_pointer(&lab, false, 240, 340);
    screen_lab_update(&lab, 1.f / 30);
    if (lab.character == CHARACTER_TESS) assert(fabsf(lab.face.tess_mood.drag_angle[1]) > 0.01f);
    screen_lab_select(&lab, LAB_OFFLINE);
    for (int i = 0; i < 120; i++) screen_lab_update(&lab, 1.f / 30);
    assert(lab.face.mode == MODE_OFFLINE);
    tap(240, 240); assert(lab.face.mode == MODE_OFFLINE && lab.face.rub_in.path == 0);
}
static void screens(int character) {
    screen_lab_init(&lab, character);
    for (int screen = 0; screen < LAB_COUNT; screen++) {
        screen_lab_select(&lab, screen);
        for (int frame = 0; frame < 45; frame++) {
            screen_lab_update(&lab, 1.f / 30);
            screen_lab_draw(&lab, &scene);
            assert(scene.n + scene.dots_n > 0 && scene.n <= R_MAX_PRIMS && scene.text_n <= R_TEXT_POOL);
            assert(scene.dots_n <= R_MAX_DOTS);
            if (character == CHARACTER_TESS) assert(scene.n < 64);
        }
        assert(!lab.face.agent.request_pending);
    }
    screen_lab_select(&lab, LAB_SPEAK);
    screen_lab_update(&lab, 1.f / 30); assert(lab.face.spk_level > 0);
    tap(80, 428); screen_lab_update(&lab, 1.f / 30); assert(lab.face.spk_level == 0);
    tap(400, 428); assert(lab.selected == -1);
    screen_lab_select(&lab, LAB_PAIR); assert(lab.face.mode == MODE_SETUP && !strcmp(lab.face.pair_code, "TEST2345"));
    screen_lab_select(&lab, LAB_LIVE);
    tap(240, 92); assert(lab.face.mode == MODE_THINKING && lab.face.live_active);
    tap(240, 92); assert(lab.face.mode == MODE_SPEAKING && lab.face.live_active);
    tap(240, 92); assert(lab.face.mode == MODE_LISTENING && lab.face.live_active);
    screen_lab_event(&lab, LAB_HOLD, 240, 240); assert(!lab.overlay);
    screen_lab_draw(&lab, &scene); face_draw(&lab.face, &native);
    assert(scene.n == native.n);
    tap(400, 92); assert(lab.selected == LAB_LIVE);
    screen_lab_event(&lab, LAB_HOLD, 240, 240); assert(lab.overlay);
    tap(400, 92); assert(lab.selected == -1);
    screen_lab_select(&lab, LAB_LIVE); screen_lab_event(&lab, LAB_KEY, 0, 0);
    assert(lab.selected == LAB_HOME && !lab.face.live_active);
}
static void events(void) {
    screen_lab_select(&lab, LAB_SETTINGS);
    tap(272, 36); assert(!lab.face.journal.open);
    for (int i = 0; i < 5; i++) tap(420, 36);
    tap(272, 36); assert(lab.face.journal.open && !lab.face.agent.open);
    tap(240, 92); assert(lab.face.journal.overlay);
    boot(); assert(!lab.face.journal.open && lab.face.menu.open);
    screen_lab_select(&lab, LAB_SETTINGS);
    assert(!lab.face.menu.service);
    lab.face.menu.sound = true;
    for (int i = 0; i < 5; i++) tap(420, 36);
    assert(lab.face.menu.service);
    screen_lab_select(&lab, LAB_EVENT_LOG);
    assert(lab.face.journal.count == JOURNAL_CAPACITY && lab.face.journal.open);
    screen_lab_pointer(&lab, true, 240, 350);
    screen_lab_pointer(&lab, true, 240, 140);
    screen_lab_pointer(&lab, false, 240, 140);
    tap(240, 140); assert(lab.face.journal.offset == 3 && !lab.face.journal.detail);
    tap(240, 175); assert(lab.face.journal.detail);
    boot(); assert(!lab.face.journal.detail && lab.face.journal.open);
    boot(); assert(!lab.face.journal.open && lab.selected == LAB_EVENT_LOG);
    boot(); assert(lab.selected == -1);
    screen_lab_select(&lab, LAB_EVENTS);
    for (int i = 0; i < 12; i++) { tap(240, 92); screen_lab_update(&lab, .1f); }
    assert(lab.face.journal.count == 13 && lab.face.journal.overlay);
    screen_lab_select(&lab, LAB_BACKGROUND); assert(lab.face.cron_running == 2);
    screen_lab_select(&lab, LAB_REMINDER); assert(lab.face.cron_due == 90);
}
static void collision_audio(void) {
    screen_lab_init(&lab, CHARACTER_TESS);
    screen_lab_select(&lab, LAB_OFFLINE);
    lab.face.grav_y = 1;
    unsigned contacts = 0;
    for (int frame = 0; frame < 150; frame++) {
        screen_lab_update(&lab, 1.f / 30);
        tess_cue_t cue; float strength, position;
        while (screen_lab_take_cue(&lab, &cue, &strength, &position)) {
            assert(cue == TC_IMPACT && strength > 0 && strength <= 1);
            assert(position >= -1 && position <= 1); contacts++;
        }
    }
    assert(contacts > 0);
    for (int screen = 0; screen < LAB_COUNT; screen++) {
        screen_lab_select(&lab, screen);
        lab.face.tess_play.cue_n = 1;
        lab.face.tess_play.cue[0].cue = TC_TOUCH;
        tess_cue_t cue; float strength, position;
        assert(screen_lab_take_cue(&lab, &cue, &strength, &position) == (screen >= LAB_GROW_POINT));
        assert(!screen_lab_cue_allowed(&lab, TC_SAD));
    }
    screen_lab_select(&lab, LAB_OFFLINE);
    lab.face.dark = true; lab.face.tess_play.cue_n = 1; lab.face.tess_play.cue[0].cue = TC_IMPACT;
    tess_cue_t cue; float strength, position;
    assert(!screen_lab_take_cue(&lab, &cue, &strength, &position));
}
static unsigned lab_waves(void) { unsigned n = 0; for (int w = 0; w < TESS_WAVES; w++) n += lab.face.tess_games.wave[w].amp != 0; return n; }
static void growth_previews_accept_natural_taps(void) {
    for (int screen = LAB_GROW_POINT; screen <= LAB_GROW_TESSERACT; screen++) {
        screen_lab_select(&lab, screen);
        for(int i=0;i<45;i++) screen_lab_update(&lab,1.f/30);  // she assembles
        tap(250,260);  // a tap on her plucks her, whatever her size
        assert(lab_waves() == 1);
        for(int i=0;i<10;i++) screen_lab_update(&lab,1.f/30);
        assert(lab.face.tess_games.shaping);
    }
}
// Each behaviour preview is under way: her own structure moves, or she is in its round.
static void assert_playing(int screen) {
    const tess_games_t *g = &lab.face.tess_games;
    bool moving = false;
    for (int frame = 0; frame < 45 && !moving; frame++) {
        screen_lab_update(&lab, 1.f / 30);
        moving = g->shaping || g->self_act || lab_waves() || g->bloom > .3f || g->game;
    }
    assert(moving);
    if (screen == LAB_PLAY_STRETCH) assert(g->game == TESS_GAME_STRETCH || g->game == TESS_GAME_FLY || g->pull_k == 0);
    if (screen == LAB_PLAY_GATHER) assert(g->game == TESS_GAME_GATHER && lab.face.tess_reaction[TR_SCATTER] > .7f);
}
static void game_previews(void) {
    static const int forms[] = {LAB_GROW_POINT, LAB_GROW_SQUARE, LAB_GROW_CUBE, LAB_GROW_TESSERACT};
    static const uint8_t progress[] = {0, 1, 7, 63};
    for (unsigned i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        screen_lab_select(&lab, forms[i]);
        assert(lab.character == CHARACTER_TESS);
        assert(lab.face.tess_games.progress == progress[i]);
        assert(!tess_games_active(&lab.face));
        assert(lab.face.tess_games.available);
        assert(!screen_lab_controls(&lab));
        screen_lab_update(&lab, 1.f / 30);
        assert_native_scene();
        assert(scene.n + scene.dots_n > 0 && scene.dots_n <= R_MAX_DOTS);
        tess_cue_t cue; float strength, position;
        assert(!screen_lab_take_cue(&lab, &cue, &strength, &position));
    }

    for (int screen = LAB_PLAY_PLUCK; screen <= LAB_PLAY_SELF; screen++) {
        screen_lab_select(&lab, screen);
        assert(lab.character == CHARACTER_TESS);
        assert(lab.face.tess_games.progress == TESS_PROGRESS_MASK);
        assert(!screen_lab_controls(&lab));
        assert_playing(screen);
        assert_native_scene();
        assert(scene.n + scene.dots_n > 0 && scene.dots_n <= R_MAX_DOTS);
        // Lab KEY replays the same behaviour, including after it has settled, without any HUD.
        for (int frame = 0; frame < 120; frame++) screen_lab_update(&lab, .1f);
        screen_lab_event(&lab, LAB_KEY, 0, 0);
        assert(lab.selected == screen);
        assert_playing(screen);
    }
    // A pluck preview sounds its shells as her body's tones; only previews may sound them, never in the dark.
    screen_lab_select(&lab, LAB_PLAY_PLUCK);
    for (int frame = 0; frame < 20; frame++) screen_lab_update(&lab, 1.f / 30);
    tess_tone_t tone;
    assert(screen_lab_take_tone(&lab, &tone) && tone.kind == TT_RING);
    lab.face.dark = true;
    lab.face.tess_play.tone[lab.face.tess_play.tone_n++] = (tess_tone_t){TT_RING, 440, .5f, .5f, 0};
    assert(!screen_lab_tones_allowed(&lab) && !screen_lab_take_tone(&lab, &tone));
    lab.face.dark = false;

    // These fixtures are developer-only, but must remain inert in a Plush build.
    screen_lab_init(&lab, CHARACTER_PLUSH);
    for (int screen = LAB_GROW_POINT; screen <= LAB_PLAY_SELF; screen++) {
        screen_lab_select(&lab, screen);
        assert(!tess_games_active(&lab.face));
        assert(lab.face.card_n > 0);
    }
}
int main(void) {
    entry(); collision_audio();
    for (int character = 0; character < 2; character++) {
        screens(character); catalog(); qr(); settings(); voice(); motion(); events();
    }
    screen_lab_init(&lab, CHARACTER_TESS);
    growth_previews_accept_natural_taps();
    game_previews();
    puts("Screen Lab: hidden entry, native QR, menus, guide, voice, gestures and bounded scenes passed");
}
