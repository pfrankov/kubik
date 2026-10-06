#include "screen_lab.h"

void screen_lab_demo_event(screen_lab_t *lab) {
    face_t *f = &lab->face;
    unsigned step = lab->event_step++ % 9;
    journal_kind_t kind = JOURNAL_STATE;
    const char *text = "Idle", *reaction = "Return to idle";
    face_card(f, NULL); f->cron_running = 0; f->cron_due = -1;
    face_set_mode(f, MODE_IDLE);
    switch (step) {
    case 0: kind = JOURNAL_LINK; text = "Agent connected"; reaction = "Ready / connection greeting"; face_set_emotion(f, EMO_HAPPY, 4); break;
    case 1: text = "Thinking"; reaction = "Waiting for agent"; face_set_mode(f, MODE_THINKING); break;
    case 2: kind = JOURNAL_ACTIVITY; text = "Here: tool / elsewhere: idle"; reaction = "Waiting pose / tool indicator"; face_set_mode(f, MODE_THINKING); face_bubble(f, BUB_TOOL, "Using tools", 3); break;
    case 3: kind = JOURNAL_EMOTION; text = "Joy"; reaction = "Character emotion reaction"; face_set_emotion(f, EMO_JOY, 4); break;
    case 4: kind = JOURNAL_SPEECH; text = "Reply voice started"; reaction = "Pose follows simulated playback"; face_set_mode(f, MODE_SPEAKING); break;
    case 5: kind = JOURNAL_SPEECH; text = "Voice stream ended"; reaction = "Finish reply, then return"; break;
    case 6: kind = JOURNAL_NOTICE; text = "Your reminder is ready. This example message is local to Screen Lab."; reaction = "Notification / text card"; face_event(f, FEV_NOTIFY, 0, 0); face_card(f, text); break;
    case 7: kind = JOURNAL_ERROR; text = "Recognition failed"; reaction = "Error indicator / failure reaction"; face_event(f, FEV_FAIL, 0, 0); face_bubble(f, BUB_ERROR, "Try again", 3); break;
    case 8: kind = JOURNAL_ACTIVITY; text = "Background work: 1 running"; reaction = "Rotating clock / no forced wake"; f->cron_running = 1; break;
    }
    event_journal_add(&f->journal, (uint32_t)f->t, kind, text, reaction);
}
void screen_lab_event_fixture(screen_lab_t *lab, int screen) {
    face_t *f = &lab->face;
    if (screen == LAB_BACKGROUND || screen == LAB_REMINDER) {
        f->cron_running = screen == LAB_BACKGROUND ? 2 : 0;
        f->cron_due = screen == LAB_REMINDER ? f->t + 90 : -1;
        f->cron_k = 1;
    } else if (screen == LAB_EVENTS) {
        f->journal.overlay = true; lab->event_step = 0;
        screen_lab_demo_event(lab);
    } else if (screen == LAB_EVENT_LOG) {
        // Longer history exercises scrolling, excerpts and tap details.
        lab->event_step = 0;
        for (int i = 0; i < 18; i++) { f->t = i * 7; screen_lab_demo_event(lab); }
        face_card(f, NULL);
        f->menu.open = true; f->menu.k = 1;
        event_journal_open(&f->journal);
    }
}
