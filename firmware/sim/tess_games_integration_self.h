// The retained idle deformations sound in their own animation window, quietly and without discoveries.
static unsigned self_frame_tones(void) {
    face_update(&face,1.f/30); face_draw(&face,&scene);
    unsigned kinds=0; tess_tone_t tone;
    while (face_take_tone(&face,&tone)) {
        if (tone.kind == TT_MOTION) { assert(tone.gain >= 0 && tone.gain <= .8f); continue; }
        assert(tone.gain>0 && tone.gain<=.1f && tone.delay_s==0);
        kinds|=1u<<tone.kind;
    }
    return kinds;
}
static void idle_choices_do_not_pull_one_corner(void) {
    // Recent touches, moods and long solitude can no longer select a one-corner stretch.
    for (int recent=0;recent<2;recent++) {
        face.tess_games.since_touch=recent?1:90;
        for (int i=0;i<200;i++) {
            unsigned action=tess_temper_self_act(&face);
            assert(action==SELF_NONE || action==SELF_PLUCK || action==SELF_SPIN || action==SELF_WRESTLE);
        }
    }
}
static void idle_deformations_are_quiet_and_synchronized(void) {
    fresh(0); face.tess_games.self_wait=0; face.tess_games.self_next=0;
    unsigned tones=self_frame_tones();
    assert(face.tess_games.self_act==SELF_WRESTLE && tones==(1u<<TT_CREAK));
    bool released=false;
    for (int i=0;i<40;i++) {
        tones=self_frame_tones();
        if (tones) {
            assert(!released && tones==(1u<<TT_TWANG) && face.tess_games.self_age>=.9f);
            released=true;
        }
        assert(face.tess_games.pull_k==0);
    }
    assert(released && !face.tess_games.progress);
    fresh(0); face.tess_games.self_wait=0; face.tess_games.self_next=1;
    assert(self_frame_tones()==(1u<<TT_RING) && waves()>0 && !face.tess_games.progress);
    fresh(0); face.tess_games.since_touch=1; face.tess_games.self_wait=0; face.tess_games.self_next=2;
    assert(self_frame_tones()==(1u<<TT_WHOOSH) && face.tess_games.self_spinning);
    idle_choices_do_not_pull_one_corner();
}
static void dimming_settles_idle_play_and_preserves_manual_inertia(void) {
    for (int act=0;act<3;act++) {
        fresh(0); face.tess_games.since_touch=1; face.tess_games.self_wait=0; face.tess_games.self_next=act;
        self_frame_tones(); face.dimmed=true; frame();
        assert(!face.tess_games.self_act && !face.tess_games.self_spinning && !waves());
        if (act==2) assert(hypotf(face.tess_mood.drag_speed[0],face.tess_mood.drag_speed[1])==0);
        face.tess_games.self_wait=0; run(3);
        assert(!face.tess_games.self_act && !waves());
    }
    fresh(0);
    for (int x=180;x<=300;x+=30) finger(true,x,255);
    finger(false,300,255);
    assert(!face.tess_games.self_spinning && hypotf(face.tess_mood.drag_speed[0],face.tess_mood.drag_speed[1])>1);
    face.dimmed=true; frame();
    assert(hypotf(face.tess_mood.drag_speed[0],face.tess_mood.drag_speed[1])>1);
}
