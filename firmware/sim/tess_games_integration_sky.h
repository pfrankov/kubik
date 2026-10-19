// Skies through the real face (included by tess_games_integration_test.c, which owns the harness): the summoning star,
// independent finger fields, persistent musical progress and each sky's own ending melody.
#include "tess_games_integration_hold.h"
static void finger_at(float x, float y, int frames) { for (int i = 0; i < frames; i++) finger(true, x, y); }

static void an_ignored_invitation_asks_nothing(void) {
    float x, y; invite(); run(1); assert(tess_sky_calling(&face, &x, &y));
    tap(480 - x, 510 - y);   // a tap away from the star is only a tap
    tap(240, 255);
    assert(face.tess_games.game != TESS_GAME_SKY && !sky());
    run(6.5f);
    assert(face.tess_games.sky_invite_t == 0 && !tess_sky_calling(&face, &x, &y) && face.tess_games.game != TESS_GAME_SKY && !sky());
    tap(x, y);
    assert(face.tess_games.game != TESS_GAME_SKY);   // and where it was, once it has gone, starts nothing
}
// A rocking swing: the pull on the glass goes to one side of level, then the other, within a second or so.
static void rock_cube(float lean, int swings, float wait_s) {
    for (int i = 0; i < swings; i++) {
        face.grav_x = i % 2 ? -lean : lean;
        run(wait_s);
    }
    face.grav_x = 0;
}
static void she_asks_only_when_lying_down_and_a_rock_summons_her(void) {
    fresh(TESS_PROGRESS_MASK); run(2);
    face.grav_x = 0; face.grav_y = 1.f;   // standing upright: she never asks, however often
    int asked = 0;
    for (int i = 0; i < 200; i++) asked += tess_sky_invite(&face);
    assert(!asked && !face.tess_games.sky_invite_t);
    face.grav_y = .25f;                   // a slight tilt of the flat cube
    for (int i = 0; i < 200 && !asked; i++) asked += tess_sky_invite(&face);
    assert(asked && face.tess_games.sky_invite_t > 0);
    fresh(TESS_PROGRESS_MASK); run(2);
    face.grav_y = 1.f; rock_cube(.3f, 4, .3f); run(.5f);   // rocked while standing: nothing
    face.grav_y = .1f; rock_cube(.3f, 2, .3f); run(1.6f);  // two swings are not yet a rock
    assert(!face.tess_games.sky_invite_t);
    rock_cube(.3f, 3, 2.f); run(.5f);                      // slow swings are not a rock either
    assert(!face.tess_games.sky_invite_t);
    face.grav_y = .1f; rock_cube(.3f, 3, .3f);             // three quick swings summon, and only summon
    assert(face.tess_games.sky_invite_t > 0 && face.tess_games.game != TESS_GAME_SKY && !sky());
    face.grav_y = 0;
}
static void raising_the_cube_answers_an_invitation_like_a_touch(void) {
    fresh(TESS_PROGRESS_MASK); face.grav_x = 0; face.grav_y = .1f; run(1);
    invite(); face.grav_y = .6f; run(.5f);                 // tipped, not raised
    assert(face.tess_games.game != TESS_GAME_SKY && face.tess_games.sky_invite_t > 0);
    face.grav_y = 1.f; frame();                            // raised
    assert(face.tess_games.game == TESS_GAME_SKY && sky() && sky()->phase == SKY_UNRAVEL);
    fresh(TESS_PROGRESS_MASK); face.grav_y = 1.f; run(1);  // raised with no invitation: nothing
    face.grav_y = .1f; run(.5f); invite(); run(7.5f); face.grav_y = 1.f; run(.5f);   // or after it has faded
    assert(face.tess_games.game != TESS_GAME_SKY); face.grav_y = 0;
}
static void the_first_sky_is_answered_by_a_finger_and_resolves_into_the_next(void) {
    answer();
    until_phase(SKY_PLAY, 4);
    assert(sky()->w.count_stars == 1 && sky()->w.count_holes == 0 && face.tess_games.sky_elements == 1);
    const sky_star_t *star = &sky()->w.star[0];
    for (int i = 0; i < 40 * 30 && sky()->phase == SKY_PLAY; i++) { finger(true, star->x, star->y); }
    face.rub_in = (rub_input_t){0};
    face.touch_in = (touch_frame_t){0};
    assert(sky()->phase == SKY_RESOLVE && face.tess_games.sky_solved == 1 && face.tess_games.sky_elements == 2);
    until_phase(SKY_PLAY, 8);   // the next sky has grown, with one thing more
    assert(sky()->w.kind == SKY_POUR && sky()->solved_here == 1 && sky()->compose == 0);
}
static void every_exit_ends_a_sky_at_once(void) {
    for (int exit = 0; exit < 9; exit++) {
        answer(); until_phase(SKY_PLAY, 4); run(1);
        switch (exit) {
        case 0: face_card(&face, "An incoming agent reply"); break;
        case 1: face.menu.open = true; break;
        case 2: face.dark = true; break;
        case 3: face_event(&face, FEV_NOTIFY, 0, 0); break;
        case 4: tess_games_set_available(&face, false); break;
        case 6: face_event(&face, FEV_TALK_START, 0, 0); break;
        case 7: face_event(&face, FEV_FAIL, 0, 0); break;
        case 8: face_event(&face, FEV_NOT_HEARD, 0, 0); break;
        default: break;
        }
        if (exit == 5) {   // a short BOOT: the stars fade and she folds back
            assert(tess_games_end_sky(&face));
            run(.5f); assert(face.tess_games.game == TESS_GAME_SKY && sky()->phase == SKY_FADE);
            run(2.5f);
        } else frame();
        assert(!face.tess_games.game && !sky());
        run(1); assert(face.tess_games.sky_lit == 0);   // no voice is left sounding
        face.dark = false; face.menu.open = false;
    }
    fresh(TESS_PROGRESS_MASK); assert(!tess_games_end_sky(&face));   // BOOT with no sky is the ordinary BOOT
}
static void what_was_learnt_is_kept_and_restored(void) {
    fresh(TESS_PROGRESS_MASK);
    face.tess_games.sky_elements = 5; face.tess_games.sky_skill = 173; face.tess_games.sky_root = 3; face.tess_games.sky_solved = 4021;
    bool settled; uint32_t kept = tess_games_sky_progress(&face, &settled);
    assert(settled && kept);
    fresh(TESS_PROGRESS_MASK);
    assert(face.tess_games.sky_elements == 0);
    tess_games_restore_sky(&face, kept);
    assert(face.tess_games.sky_elements == 5 && face.tess_games.sky_skill == 173 && face.tess_games.sky_root == 3 && face.tess_games.sky_solved == 4021);
    tess_games_restore_sky(&face, 15u | 7u << 12);   // nonsense is not taken
    assert(face.tess_games.sky_elements == 5 && face.tess_games.sky_root == 3);
    tess_games_restore(&face, TESS_PROGRESS_MASK);   // a reset forgets it
    assert(face.tess_games.sky_elements == 0 && face.tess_games.sky_solved == 0);
}
static void the_invitation_stays_on_the_body(void) {
    static const unsigned forms[] = {0, 1, 7, TESS_PROGRESS_MASK};
    for (unsigned form = 0; form < sizeof forms / sizeof *forms; form++) {
        fresh(forms[form]);
        for (int n = 0; n < 32 && !tess_sky_invite(&face); n++) {}
        assert(sky_summon_shown(&face) == 0);
        unsigned vertex = face.tess_games.sky_call_vertex;
        for (int n = 0; n < 45; n++) {
            float expected_x = face.tess_games.vertex_px[vertex][0];
            float expected_y = face.tess_games.vertex_px[vertex][1];
            frame();
            float x, y; assert(tess_sky_calling(&face, &x, &y));
            assert(x == expected_x && y == expected_y);
            assert(x >= 24 && x <= 456 && y >= 24 && y <= 456);
        }
        assert(sky_summon_shown(&face) > .99f);
    }
}

static void dimming_cancels_a_visible_invitation(void) {
    float x, y; invite(); run(1);
    assert(tess_sky_calling(&face, &x, &y));
    face.dimmed = true; frame();
    assert(!tess_sky_calling(&face, &x, &y));
    face.dimmed = false; frame();
    assert(!tess_sky_calling(&face, &x, &y)); // brightening cannot revive an expired offer
    invite(); run(1); assert(tess_sky_calling(&face, &x, &y));
    face.dimmed = true;
    assert(!tess_sky_tap(&face, x, y) && !sky()); // input can arrive before the next tick
    face.grav_y = .9f;
    assert(!sky_summon_tick(&face, 1.f / 30) && !sky()); // dim and lift in the same tick
    assert(!tess_sky_calling(&face, &x, &y));
}

static void the_invitation_is_answered_on_the_body(void) {
    float x0, y0, x, y;
    invite(); assert(tess_sky_calling(&face, &x0, &y0));
    run(1.5f); assert(tess_sky_calling(&face, &x, &y));
    tap(x + 30, y - 25);
    assert(face.tess_games.game == TESS_GAME_SKY && sky() && sky()->phase == SKY_UNRAVEL);
}

static sky_world_t *hand_made_sky(void) {
    answer(); until_phase(SKY_PLAY, 4);
    sky_world_t *w = &sky()->w;
    *w = (sky_world_t){.feed_scale=1,.rng=71,.count_stars=2,.count_holes=1,
        .source={.x=90,.y=140,.dx=1,.speed=120},
        .star={{.x=240,.y=255,.ax=240,.ay=255,.hz=440}, {.x=360,.y=365,.ax=360,.ay=365,.hz=587}},
        .hole={{.x=140,.y=355,.r=125,.spin=-1}}};
    sky()->hint_star=-1;
    return w;
}
static void each_finger_makes_a_temporary_hole_everywhere(void) {
    sky_world_t *w=hand_made_sky();
    const sky_source_t source=w->source; const sky_hole_t hole=w->hole[0];
    finger_at(240,255,6); assert(sky()->touch[0].down && sky()->touch[0].shown==1);
    finger_at(90,140,3); finger_at(140,355,3);
    assert(w->source.x==source.x && w->source.dx==source.dx && w->hole[0].x==hole.x && w->hole[0].y==hole.y);
    assert(w->star[0].ax==240 && w->star[0].ay==255);
    face_card(&face,"end"); frame(); assert(!sky());
}
static void finger_fields_release_independently(void) {
    sky_world_t *w=hand_made_sky();
    finger_at(150,290,6);
    face.touch_in.point[1]=(touch_point_t){.x=350,.y=220,.down=true,.id=15};
    for(int i=0;i<6;i++) frame();
    assert(sky()->touch[0].down && sky()->touch[1].down && sky()->touch[1].shown==1);
    face.touch_in.point[0].down=false;
    for(int i=0;i<10;i++) frame();
    assert(sky()->touch[0].shown==0 && sky()->touch[1].shown==1);
    assert(sky()->touch[1].x==350 && sky()->touch[1].y==220);
    face.touch_in=(touch_frame_t){0}; face.rub_in=(rub_input_t){0}; run(.3f);
    assert(!sky()->finger_down && sky()->touch[0].shown==0 && sky()->touch[1].shown==0);
    w->star[0].fill=1; sky()->struggle=999; run(.1f);
    assert(sky()->hint_star==1);
    face_card(&face,"end"); frame(); assert(!sky());
}
static void aurora_ends_without_goals_or_progress(void) {
    sky_world_t *w=hand_made_sky();
    w->kind=SKY_AURORA; w->count_stars=0; w->t=39.8f;
    unsigned solved=face.tess_games.sky_solved, lessons=face.tess_games.sky_elements;
    finger_at(200,300,9);
    assert(sky()->phase==SKY_RESOLVE && sky()->hint_star==-1 && !sky()->played.n);
    assert(face.tess_games.sky_solved==solved && face.tess_games.sky_elements==lessons);
    for(int i=0;i<SKY_STARS;i++) assert(sky()->melody.tour[i]==-1);
    for(int i=0;i<sky()->melody.n;i++) assert(sky()->melody.note[i].star==-1);
    face_card(&face,"end"); frame(); assert(!sky());
}
static void stars_can_be_lit_one_after_another(void) {
    sky_world_t *w=hand_made_sky();
    w->count_holes=0; w->count_stars=3;
    w->star[2]=(sky_star_t){.x=100,.y=340,.ax=100,.ay=340,.hz=659,.degree=4};
    const int order[]={2,0,1};
    for(int n=0;n<3;n++) {
        sky_star_t *star=&w->star[order[n]];
        for(int i=0;i<SKY_PARTICLES;i++) sky()->p[i]=(sky_particle_t){.x=(int16_t)(star->x*SKY_Q3),.y=(int16_t)(star->y*SKY_Q3)};
        run(.4f);
        assert(star->fill==1 && star->sang);
        for(int j=0;j<=n;j++) assert(w->star[order[j]].fill==1);
    }
    run(1.7f);
    assert(sky()->phase==SKY_RESOLVE && sky()->played.n==3);
    for(int i=0;i<3;i++) assert(sky()->played.order[i]==order[i]);
    face_card(&face,"end"); frame(); assert(!sky());
}
// Solved, the sky sings back how it was played: every star's note in the order lit, its star flashing, then its chord.
static void every_sky_sings_its_own_melody(void) {
    answer(); until_phase(SKY_PLAY, 4);
    const sky_star_t *star = &sky()->w.star[0];
    for (int i = 0; i < 40 * 30 && sky()->phase == SKY_PLAY; i++) finger(true, star->x, star->y);
    finger(false, 0, 0);
    assert(sky()->phase == SKY_RESOLVE && sky()->melody.n >= 6);
    assert(sky()->played.n == 1 && sky()->played.order[0] == 0 && sky()->melody.tour[0] == 0);   // sung as it was played
    float len = sky()->resolve_len, glowed = 0;
    assert(len > SKY_MELODY_AT_S + 2.2f && len < SKY_MELODY_AT_S + 4.3f + SKY_CHORD_RING_S);
    tones_heard = 0;
    for (int i = 0; i < (int)(len * 30) - 2 && sky()->phase == SKY_RESOLVE; i++) { frame(); glowed = fmaxf(glowed, sky()->w.star[0].glow); }
    assert(sky()->melody_next == sky()->melody.n && sky()->tour_lit == 1 && glowed > .9f);
    assert((tones_heard & (1u << TT_SKY_GAME_PLUCK)) && (tones_heard & (1u << TT_SKY_GAME_CHIME)));
    face_card(&face, "end"); frame(); assert(!sky());
}
static void assert_early_exit_tones_once(void) {
    tess_tone_t tone;
    unsigned releases = 0, plucks = 0;
    while (face_take_tone(&face, &tone)) {
        releases += tone.kind == TT_SKY_RELEASE;
        plucks += tone.kind == TT_SKY_GAME_PLUCK;
    }
    assert(releases == 1 && plucks == 1);
    tess_sky_tick(&face, 1.f / 30);
    assert(!face.tess_games.sky_audio && !face_take_tone(&face, &tone));
}
static void leave_before_first_held_note(bool restore) {
    answer(); until_phase(SKY_PLAY, 4);
    assert(face.tess_games.sky_audio && face.tess_games.sky_lit == 0);
    face.tess_play.tone_n = 0;
    tess_tone(&face, TT_SKY_GAME_PLUCK, 440, .4f, .2f);
    if (restore) tess_games_restore(&face, TESS_PROGRESS_MASK);
    else face_event(&face, FEV_NOTIFY, 0, 0);
    assert(!sky() && face.tess_games.sky_lit == 0);
    if (!restore) assert(face.tess_games.sky_audio); // the ordinary leave waits for the every-frame sound cleanup
    tess_sky_tick(&face, 1.f / 30);
    assert(!face.tess_games.sky_audio);
    assert_early_exit_tones_once();
}
static void exits_before_first_held_note_release_exactly_once(void) {
    leave_before_first_held_note(false); // notification leaves a live audio owner for tess_sky_tick
    leave_before_first_held_note(true);  // reset releases directly; the following tick must not repeat it
}
static void quiet_and_ordinary_handling_keep_the_game_open(void) {
    sky_world_t *w = hand_made_sky();
    w->count_stars = 1;
    w->star[0].x = w->star[0].ax = 430;
    w->star[0].y = w->star[0].ay = 430;
    w->count_holes = 0;
    face.grav_x = face.grav_y = 0;
    run(18);
    assert(sky() && sky()->phase == SKY_PLAY);
    face_event(&face, FEV_PICKUP, 0, 0); frame();
    assert(sky() && sky()->phase == SKY_PLAY);
    face.grav_x = .4f; face.grav_y = -.3f; run(1);
    assert(sky() && sky()->phase == SKY_PLAY);
    face_event(&face, FEV_SHAKE, 0, 0); frame();
    assert(sky() && sky()->phase == SKY_FADE);
    run(2);
    assert(!sky() && !face.tess_games.sky_audio);
}
static void a_visit_does_not_end_after_six_skies(void) {
    hand_made_sky();
    sky()->solved_here = 6;
    sky()->phase = SKY_RESOLVE;
    sky()->resolve_len = .1f;
    until_phase(SKY_PLAY, 8);
    assert(sky()->solved_here == 6);
    sky()->solved_here = UINT8_MAX;
    for (int i = 0; i < sky()->w.count_stars; i++) sky()->w.star[i].fill = 1;
    sky()->hold = SKY_HOLD_S;
    frame();
    assert(sky()->phase == SKY_RESOLVE && sky()->solved_here == UINT8_MAX);
    face_card(&face, "end"); frame();
}
typedef struct { int pulses, quiet; bool low_pad, upper_pad; } sky_sound_observation_t;
static void observe_sky_tones(sky_sound_observation_t *heard, int frame) {
    tess_tone_t tone;
    while (face_take_tone(&face, &tone)) {
        if (tone.kind == TT_SKY_GAME_PULSE) {
            assert(tone.hz > 140 && tone.hz < 250 && tone.gain <= .6f);
            if (frame < 90) heard->pulses++; else if (frame > 100) heard->quiet++;
        }
        if (tone.kind == TT_SKY_PAD && tone.gain > 0) {
            heard->low_pad |= tone.hz < 250; heard->upper_pad |= tone.hz > 290;
        }
    }
}
static void crossings_build_a_low_sparse_rhythm(void) {
    hand_made_sky();
    tess_sky_t *k = sky();
    k->w.star[0].fill = .3f; k->w.star[1].fill = 1;
    k->w.star[0].hz = 293.665f; k->w.star[1].hz = 493.883f;
    tess_tone_t tone;
    while (face_take_tone(&face, &tone)) {}
    sky_hits_t hits = {0};
    sky_sound_observation_t heard = {0};
    for (int frame = 0; frame < 180; frame++) {
        hits.inside[0] = frame < 90 ? 12 : 0;
        sky_sound_frame(&face, k, &hits, 1.f / 30);
        observe_sky_tones(&heard, frame);
    }
    assert(heard.pulses >= 2 && heard.pulses <= 6 && !heard.quiet && heard.low_pad && heard.upper_pad);
    k->phase = SKY_FADE; hits.inside[0] = 112; k->beat_wait = 0;
    sky_sound_frame(&face, k, &hits, 1.f / 30);
    while (face_take_tone(&face, &tone)) assert(tone.kind != TT_SKY_GAME_PULSE);
    face_card(&face, "end"); frame();
}
static void skies_summon_play_and_sing(void) {
    diagonal_holds_start_skies();
    crossings_build_a_low_sparse_rhythm();
    quiet_and_ordinary_handling_keep_the_game_open();
    a_visit_does_not_end_after_six_skies();
    the_invitation_stays_on_the_body();
    dimming_cancels_a_visible_invitation();
    the_invitation_is_answered_on_the_body();
    exits_before_first_held_note_release_exactly_once();
    each_finger_makes_a_temporary_hole_everywhere();
    finger_fields_release_independently();
    aurora_ends_without_goals_or_progress();
    stars_can_be_lit_one_after_another();
    every_sky_sings_its_own_melody();
}

static void pouring_uses_gravity_without_rest_pose_drift(void) {
    for(int kind=SKY_PUZZLE;kind<=SKY_POUR;kind++) {
        answer(); until_phase(SKY_PLAY,4);
        tess_sky_t *k=sky(); k->w.kind=(uint8_t)kind;
        face.grav_x=.3f; face.grav_y=-.2f;
        face.tess_games.tilt[0]=-.6f; face.tess_games.tilt[1]=.8f;
        sky_world_t expected=k->w;
        sky_particle_t particles[SKY_PARTICLES]; memcpy(particles,k->p,sizeof particles);
        float scale=kind==SKY_POUR?1.f:SKY_TILT_SCALE;
        sky_force_t force={.gx=face.grav_x*scale,.gy=face.grav_y*scale};
        sky_stream_step(&expected,particles,SKY_PARTICLES,1.f/30,&force,NULL);
        tess_sky_update(&face,1.f/30);
        assert(!memcmp(particles,k->p,sizeof particles));
        face_card(&face,"end"); frame();
    }
}

static void quiet_releases_survive_a_full_queue(void) {
    fresh(63);
    face.tess_play.tone_n = 0;
    for (int i = 0; i < TESS_TONES; i++) tess_tone(&face, TT_SKY_INVITE, 440, .5f, 0);
    assert(face.tess_play.tone_n == TESS_TONES);
    face.dimmed = true;
    tess_tone_slot(&face, TT_SKY_PAD, 0, 440, 0, 0, 0);
    tess_tone_slot(&face, TT_SKY_HUM, 1, 220, 0, 0, 0);
    tess_tone_slot(&face, TT_SKY_PAD, 0, 440, 0, 0, 0);
    tess_tone_t tone;
    unsigned releases = 0;
    while (face_take_tone(&face, &tone)) { assert(tone.gain == 0); releases++; }
    assert(releases == 2 && !face.tess_play.tone_n);
    face.dark = true;
    tess_tone_slot(&face, TT_SKY_PAD, 2, 440, 0, 0, 0);
    assert(face_take_tone(&face, &tone) && tone.gain == 0 && tone.slot == 2);
    assert(!face_take_tone(&face, &tone));
    face.tess_play.tone_n = 0;
    for (int i = 0; i < TESS_TONES; i++) tess_tone(&face, TT_SKY_GAME_PLUCK, 440, .5f, 0);
    tess_tone(&face, TT_SKY_RELEASE, 440, 0, 0);
    assert(face_take_tone(&face, &tone) && tone.kind == TT_SKY_RELEASE && tone.gain == 0);
    assert(!face_take_tone(&face, &tone) && !face.tess_play.tone_n);
}
