// Hidden Skies entry through complete touch samples and the real face update, including primary-finger ownership.
static void corners(int diagonal, bool swapped) {
    touch_point_t a = {.x=40,.y=diagonal ? 440 : 40,.down=true,.id=7};
    touch_point_t b = {.x=440,.y=diagonal ? 40 : 440,.down=true,.id=12};
    face.touch_in = (touch_frame_t){.point={swapped ? b : a, swapped ? a : b}};
    face.rub_in = (rub_input_t){.down=true,.x=face.touch_in.point[0].x,.y=face.touch_in.point[0].y};
}
static void release_corners(void) {
    face.touch_in = (touch_frame_t){0}; face.rub_in = (rub_input_t){0}; frame();
}
static void either_diagonal_starts_a_sky_without_an_invitation(void) {
    for (int diagonal=0; diagonal<2; diagonal++) for (int swapped=0; swapped<2; swapped++) {
        fresh(0); face.grav_y=1; // upright also works; no invitation or unlocked progress needed
        corners(diagonal, swapped); run(1.4f);
        assert(!sky() && face.tess_games.sky_hold_owned && face.tess_games.game==TESS_GAME_NONE);
        assert(face.tess_games.pull_k==0 && face.rub.stage==0);
        // Controller report reordering does not interrupt the same two physical contacts.
        corners(diagonal, !swapped); run(.2f);
        assert(sky() && sky()->phase==SKY_UNRAVEL);
        uint32_t round=face.tess_games.round; run(2);
        assert(sky() && face.tess_games.round==round); // holding cannot restart the game
        release_corners();
    }
}
static void the_second_corner_takes_over_a_stretch_without_a_throw(void) {
    fresh(0); finger(true,40,40); run(.2f);
    assert(face.tess_games.game==TESS_GAME_STRETCH);
    corners(0,false); run(.8f);
    assert(!sky() && face.tess_games.game==TESS_GAME_NONE);
    face.touch_in.point[1].down=false; run(.3f);
    corners(0,false); run(2);
    assert(!sky() && face.tess_games.game==TESS_GAME_NONE); // interrupted: full release required
    release_corners(); tap(40,40);
    assert(!face.tess_games.game && !face.tess_taps);
    finger(true,375,255);
    assert(face.tess_games.game==TESS_GAME_STRETCH); // ordinary single-finger play is restored
    release_corners();
    fresh(0); corners(1,false); run(1); release_corners();
    assert(face.tess_games.finger_owned); tap(240,255);
    assert(!face.tess_taps && waves()==0); // a late release tap also belongs to the canceled hold
    corners(1,false); run(.7f); assert(!sky()); run(.9f); assert(sky());
}
static void a_diagonal_needs_two_continuous_corner_contacts(void) {
    for (int invalid=0; invalid<6; invalid++) {
        fresh(0); corners(0,false);
        switch (invalid) {
        case 0: face.touch_in.point[1].down=false; break;
        case 1: face.touch_in.point[1].y=40; break; // adjacent corners
        case 2: face.touch_in.point[1].x=240; break;
        case 3: face.touch_in.point[1].id=7; break; // duplicated contact
        case 4: face.touch_in.point[1].x=480; break;
        case 5: face.touch_in.point[1].y=-1; break;
        }
        run(1.8f); assert(!sky());
    }
    fresh(0); corners(0,false); run(1);
    face.touch_in.point[1].id=15; run(.7f); assert(!sky()); run(.9f); assert(sky());
    fresh(0); corners(0,false); run(.8f);
    face.touch_in.point[1].x=300; frame(); corners(0,false); run(2);
    assert(!sky()); // moving out cancels, not pauses, the hold
}
static void corner_holds_respect_the_current_screen_and_agent(void) {
    for (int blocked=0; blocked<11; blocked++) {
        fresh(0); corners(0,false);
        switch (blocked) {
        case 0: face.dimmed=true; break;
        case 1: face.dark=true; break;
        case 2: face.menu.open=true; break;
        case 3: face_set_mode(&face,MODE_SETUP); break;
        case 4: face_set_mode(&face,MODE_THINKING); break;
        case 5: face_set_mode(&face,MODE_SPEAKING); break;
        case 6: face.live_active=true; break;
        case 7: face_card(&face,"Agent reply"); break;
        case 8: tess_games_set_available(&face,false); break;
        case 9: tess_scatter_begin(&face); break;
        case 10: face_set_character(&face,CHARACTER_PLUSH); break;
        }
        run(1.8f); assert(!sky());
    }
    fresh(0); corners(0,false); run(.8f);
    face.menu.open=true; frame(); face.menu.open=false; face.menu.k=0; run(2);
    assert(!sky()); release_corners();
    // Returning from a menu cannot complete an old partial hold.
    corners(0,false); run(1.6f); assert(sky());
    fresh(0); corners(1,false); run(.8f);
    face_set_mode(&face,MODE_THINKING); frame();
    assert(face.tess_games.sky_hold_owned && !tess_games_active(&face));
    face_set_mode(&face,MODE_IDLE); run(2); assert(!sky()); release_corners();
}
static void diagonal_holds_start_skies(void) {
    either_diagonal_starts_a_sky_without_an_invitation();
    the_second_corner_takes_over_a_stretch_without_a_throw();
    a_diagonal_needs_two_continuous_corner_contacts();
    corner_holds_respect_the_current_screen_and_agent();
}
