// The retained idle deformations sound in their own animation window, quietly and without discoveries.
static unsigned self_frame_tones(void) {
    face_update(&face,1.f/30); face_draw(&face,&scene);
    unsigned kinds=0; tess_tone_t tone;
    while (face_take_tone(&face,&tone)) {
        assert(tone.gain>0 && tone.gain<=.1f && tone.delay_s==0);
        kinds|=1u<<tone.kind;
    }
    return kinds;
}
static void idle_choices_do_not_pull_corners(void) {
    // Recent touches and long solitude never select either a one-corner or an opposing-corner stretch.
    for (int recent=0;recent<2;recent++) {
        face.tess_games.since_touch=recent?1:90;
        for (int i=0;i<200;i++) {
            unsigned action=tess_temper_self_act(&face);
            assert(action==SELF_NONE || action==SELF_PLUCK || action==SELF_SPIN);
        }
    }
}
static void idle_deformations_are_quiet_and_synchronized(void) {
    fresh(0); face.tess_games.self_wait=0; face.tess_games.self_next=0;
    assert(self_frame_tones()==(1u<<TT_RING) && waves()>0);
    for (int i=0;i<40;i++) {
        unsigned tones=self_frame_tones();
        assert(!(tones&((1u<<TT_CREAK)|(1u<<TT_TWANG)))); // no autonomous stretch/release sound
        assert(face.tess_games.pull_k==0);
    }
    assert(!face.tess_games.progress);
    fresh(0); face.tess_games.since_touch=1; face.tess_games.self_wait=0; face.tess_games.self_next=1;
    assert(self_frame_tones()==(1u<<TT_WHOOSH) && face.tess_games.self_spinning);
    idle_choices_do_not_pull_corners();
}
static void dimming_settles_idle_play_and_preserves_manual_inertia(void) {
    for (int act=0;act<2;act++) {
        fresh(0); face.tess_games.since_touch=1; face.tess_games.self_wait=0; face.tess_games.self_next=act;
        self_frame_tones(); face.dimmed=true; frame();
        assert(!face.tess_games.self_spinning && !waves());
        if (act==1) assert(hypotf(face.tess_mood.drag_speed[0],face.tess_mood.drag_speed[1])==0);
        face.tess_games.self_wait=0; run(3);
        assert(!face.tess_games.self_spinning && !waves());
    }
    fresh(0);
    for (int x=180;x<=300;x+=30) finger(true,x,255);
    finger(false,300,255);
    assert(!face.tess_games.self_spinning && hypotf(face.tess_mood.drag_speed[0],face.tess_mood.drag_speed[1])>1);
    face.dimmed=true; frame();
    assert(hypotf(face.tess_mood.drag_speed[0],face.tess_mood.drag_speed[1])>1);
}
