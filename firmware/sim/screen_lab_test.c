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
    tap(240, 192); assert(lab.selected == LAB_CATCH_2);
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
    tap(240, 300); assert(!strcmp(lab.face.agent.selected_model, "demo/model-1"));
    boot(); assert(lab.face.agent.view == AGENT_VIEW_OVERVIEW);
    boot(); assert(!lab.face.agent.open);
    tap(356, 226); assert(lab.face.agent.view == AGENT_VIEW_GUIDE);
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
    tap(240, 300); boot(); assert(!strcmp(lab.face.agent.stt_model, "Detailed"));
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
        assert(!screen_lab_take_cue(&lab, &cue, &strength, &position));
    }
    screen_lab_select(&lab, LAB_OFFLINE);
    lab.face.dark = true; lab.face.tess_play.cue_n = 1; lab.face.tess_play.cue[0].cue = TC_IMPACT;
    tess_cue_t cue; float strength, position;
    assert(!screen_lab_take_cue(&lab, &cue, &strength, &position));
}
static void growth_previews_accept_natural_taps(void) {
    for (int screen = LAB_GROW_POINT; screen <= LAB_GROW_TESSERACT; screen++) {
        screen_lab_select(&lab, screen);
        for (int n = 0; n < 3; n++) {
            tap(240, 255);
            for (int i = 0; i < 9; i++) screen_lab_update(&lab, 1.f / 30);
        }
        for (int i = 0; i < 45; i++) screen_lab_update(&lab, 1.f / 30);
        assert(tess_games_active(&lab.face) && lab.face.tess_games.game == TESS_GAME_ECHO);
    }
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

    static const int game_screens[] = {
        LAB_ECHO_0, LAB_ECHO_1, LAB_ECHO_2,
        LAB_CATCH_0, LAB_CATCH_1, LAB_CATCH_2,
    };
    static const tess_game_t games[] = {
        TESS_GAME_ECHO, TESS_GAME_ECHO, TESS_GAME_ECHO,
        TESS_GAME_CATCH, TESS_GAME_CATCH, TESS_GAME_CATCH,
    };
    for (unsigned i = 0; i < sizeof game_screens / sizeof game_screens[0]; i++) {
        screen_lab_select(&lab, game_screens[i]);
        assert(lab.character == CHARACTER_TESS);
        assert(lab.face.tess_games.progress == TESS_PROGRESS_MASK);
        assert(tess_games_active(&lab.face));
        assert(lab.face.tess_games.game == games[i]);
        assert(lab.face.tess_games.tier == i % 3);
        assert(!screen_lab_controls(&lab));
        if (games[i] == TESS_GAME_ECHO) {
            // A pattern tap is ignored; once the sequence reaches its input
            // phase, the existing raw Lab tap route advances the game step.
            unsigned step = lab.face.tess_games.step;
            tap(240, 255);
            assert(lab.face.tess_games.step == step);
            for (int frame = 0; frame < 50 && lab.face.tess_games.phase != TESS_GAME_WAIT; frame++)
                screen_lab_update(&lab, .1f);
            assert(lab.face.tess_games.phase == TESS_GAME_WAIT);
            step = lab.face.tess_games.step;
            tap(240, 255);
            assert(lab.face.tess_games.step == step + 1);
        }
        screen_lab_update(&lab, 1.f / 30);
        assert_native_scene();
        assert(scene.n + scene.dots_n > 0 && scene.dots_n <= R_MAX_DOTS);
        tess_cue_t cue; float strength, position;
        assert(!screen_lab_take_cue(&lab, &cue, &strength, &position));

        // Lab KEY is a deterministic escape/replay affordance. It can start
        // the same fixture after its natural timeout without adding a HUD.
        for (int frame = 0; frame < 250 && tess_games_active(&lab.face); frame++)
            screen_lab_update(&lab, .1f);
        assert(!tess_games_active(&lab.face));

        // KEY deterministically restarts the selected debug tier; no extra HUD control is needed.
        screen_lab_event(&lab, LAB_KEY, 0, 0);
        assert(lab.selected == game_screens[i]);
        assert(tess_games_active(&lab.face));
        assert(lab.face.tess_games.game == games[i]);
        assert(lab.face.tess_games.tier == i % 3);
    }

    // These fixtures are developer-only, but must remain inert in a Plush build.
    screen_lab_init(&lab, CHARACTER_PLUSH);
    for (int screen = LAB_GROW_POINT; screen <= LAB_CATCH_2; screen++) {
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
