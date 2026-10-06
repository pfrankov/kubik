#include "face_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "font.h"
#include "sprite.h"
#include "tess.h"
#include "face_math.h"
#include "ui_text.h"

// ---------------------------------------------------------------- API

emotion_t face_emotion_from_name(const char *n) {
    static const char *names[EMO_COUNT] = {"neutral", "happy", "joy", "love", "sad", "angry", "surprised",
                                           "confused", "sleepy", "thinking", "wink", "shy", "proud", "dizzy"};
    for (int i = 0; i < EMO_COUNT; i++)
        if (n && strcmp(n, names[i]) == 0) return (emotion_t)i;
    return EMO_NEUTRAL;
}

static void draw_live_recording(const face_t *f, scene_t *s) {
    const uint32_t mint = 0x62E5C8, ink = 0x15171B;
    sc_rbox(s, 240, 414, 170, 25, 25, 0, mint, 1);
    sc_icon(s, ICON_MIC, 104, 414, 30, ink, 1);
    sc_label(s, TXT_ACTION, "Listening", 130, 414, -1, ink, 1);
    // Level bars stay visible in silence; only measured microphone energy moves them.
    static const float weights[] = {.45f, .7f, .9f, 1.f, .9f, .7f, .45f};
    float level = f->mic_level;
    for (int i = 0; i < 7; i++) {
        float half_height = 1 + 12 * clampf(level, 0, 1) * weights[i];
        sc_rbox(s, 314 + i * 10, 414, 2.5f, half_height, 1, 0, ink, .85f);
    }
}

static void draw_live_status(const face_t *f, scene_t *s) {
    if (!f->live_active) return;
    const uint32_t gold = 0xFFC868, ink = 0xF2EADC;
    sc_label(s, TXT_ACTION, "GPT Live", 240, 36, 0, gold, 1);
    if (f->live_ready && f->live_mic) draw_live_recording(f, s);
    else {
        const char *status = !f->live_ready ? "Connecting" : "Microphone unavailable";
        int icon = !f->live_ready ? ICON_PLUG :
                   f->mode == MODE_SPEAKING ? ICON_AUDIO_LINES : ICON_BRAIN;
        float left = 240 - (label_width(TXT_ACTION, status) + 42) * .5f;
        sc_icon(s, icon, left + 15, 414, 30, gold, 1);
        sc_label(s, TXT_ACTION, status, left + 42, 414, -1, gold, 1);
    }
    sc_label(s, TXT_ACTION, "KEY to end", 240, 452, 0, ink, .8f);
}

static void draw_menu_screen(face_t *f, scene_t *s) {
    if (f->status.open) draw_status(&f->status, s);
    else if (f->journal.open) event_journal_draw(&f->journal, s);
    else if (f->agent.open) face_agent_draw(s, &f->agent, f->character == CHARACTER_TESS);
    else {
        draw_menu(f, s);
        draw_bubble(f, s);
    }
}


void face_init(face_t *f) {
    memset(f, 0, sizeof(*f));
    f->rng = 0x1234567u;
    f->mode = MODE_BOOT;
    f->emotion = EMO_NEUTRAL;
    f->emotion_left = -1;
    f->next_blink = 3;
    f->next_saccade = 2;
    f->next_micro = 14;
    base_params(&f->cur);
    f->cur.v[P_LTL] = f->cur.v[P_LTR] = 1;
    f->target = f->cur;
    if (face_character(f) == CHARACTER_TESS) tess_reset(f);
    f->body_breath = 1;
    f->body.cur = -1;
    f->menu.pressed = -1;
    agent_menu_reset(&f->agent);
    f->cron_due = -1;
    f->body.req = -1;
    f->body.next_idle = 4;
    f->body.exit_to = -1;
    f->sent_t = -1;
    f->pickup_t = -1;
    f->scr_cx = 240;
    f->scr_cy = 200;
    f->scr_hw = 80;
    f->scr_hh = 68;
    f->scr_vis = 1;
    if (face_character(f) == CHARACTER_PLUSH) {
        for (int i = 0; i < BA_COUNT; i++) f->body.id[i] = (int8_t)sprite_find(k_body_names[i]);
        if (f->body.id[BA_IDLE] < 0) f->body.id[BA_IDLE] = (int8_t)(sprite_ready() ? 0 : -1);
        if (sprite_ready()) body_graph(&f->body);
        body_request(f, BA_WAVE);
    }
}


#ifndef ESP_PLATFORM
// Tess starts as a spark and unfolds into the tesseract (tess_assemble).
void face_set_character(face_t *f, character_t character) {
    if ((unsigned)character >= CHARACTER_COUNT || face_character(f) == character) return;
    f->character = character;
    memset(&f->body, 0, sizeof f->body);
    memset(f->parts, 0, sizeof f->parts);
    f->body.cur = f->body.req = f->body.exit_to = -1;
    f->body.next_idle = 4;
    f->body_breath = 1;
    f->body_dx = f->body_dy = f->body_vx = 0;
    f->loose_x = f->loose_y = f->loose_vx = f->loose_vy = 0;
    tess_reset(f);
    for (int i = 0; i < BA_COUNT; i++)
        f->body.id[i] = character == CHARACTER_PLUSH ? (int8_t)sprite_find(k_body_names[i]) : -1;
    if (character == CHARACTER_PLUSH) body_graph(&f->body);
}


#endif

void face_set_mode(face_t *f, face_mode_t m) {
    if (f->mode == m) return;
    face_mode_t old = f->mode;
    f->mode = m;
    f->mode_t = 0;
    f->idle_t = 0;
    f->micro_left = 0;
    if (face_character(f) == CHARACTER_TESS) {
        tess_set_mode(f, old, m);
        return;
    }
    if (m == MODE_LISTENING) {
        f->hop_v -= 90;  // perk up
        f->look_tx = f->look_ty = 0;
    }
    if (old == MODE_LISTENING) {
        f->sent_t = 0;  // the outline retracts; sent to the server: the phrase flies off (draw_listening)
        if (m == MODE_THINKING) f->hop_v -= 60;
    }
    if (m == MODE_IDLE && old == MODE_SLEEP) f->hop_v -= 120;
    if (m == MODE_SPEAKING && f->emotion == EMO_THINKING) f->emotion = EMO_NEUTRAL;
}

void face_set_emotion(face_t *f, emotion_t e, float seconds) {
    if (face_character(f) == CHARACTER_TESS && (f->mode == MODE_OFFLINE || f->tess_fallen)) return;
    f->emotion = e;
    f->emotion_left = seconds > 0 ? seconds : -1;
    if (face_character(f) == CHARACTER_TESS) {
        tess_emotion(f, e, seconds);
        return;
    }
    static const uint8_t gesture[EMO_COUNT] = {
        [EMO_HAPPY] = BA_JOY + 1, [EMO_JOY] = BA_JOY + 1, [EMO_PROUD] = BA_JOY + 1,
        [EMO_SURPRISED] = BA_SURPRISED + 1, [EMO_DIZZY] = BA_DIZZY + 1,
        [EMO_ANGRY] = BA_ANGRY + 1, [EMO_CONFUSED] = BA_THINK + 1,
        [EMO_THINKING] = BA_THINK + 1, [EMO_WINK] = BA_WAVE + 1,
    };
    if ((unsigned)e < EMO_COUNT && gesture[e]) body_request(f, (body_anim_t)(gesture[e] - 1));
    if (e == EMO_JOY || e == EMO_PROUD || e == EMO_SURPRISED) f->hop_v -= 140;
    if (e == EMO_DIZZY) f->dizzy_t = seconds > 0 ? seconds : 2.5f;
}

void face_spawn(face_t *f, int kind, float x, float y) {
    for (int i = 0; i < FACE_MAX_PARTICLES; i++) {
        particle_t *p = &f->parts[i];
        if (p->life > 0) continue;
        p->kind = (uint8_t)kind;
        p->x = x;
        p->y = y;
        p->vx = frange(f, -14, 14);
        p->vy = kind == 1 ? 0 : frange(f, -60, -38);
        p->max_life = p->life = kind == 1 ? frange(f, 0.7f, 1.2f) : frange(f, 1.6f, 2.4f);
        p->size = kind == 0 ? frange(f, 20, 30) : kind == 1 ? frange(f, 10, 18) : frange(f, 14, 20);
        p->spin = frange(f, -0.6f, 0.6f);
        return;
    }
}

static void face_tap(face_t *f, float x, float y) {
    // Tapping an eye makes it squeeze; tapping elsewhere tickles.
    if (has_body(f)) {
        // panel -> face space (inverse of the transform used in face_draw)
        float k = fminf(f->scr_hw / 185.f, f->scr_hh / 135.f);
        float dx = x - f->scr_cx, dy = y - f->scr_cy;
        float c = cosf(f->scr_ang), sn = sinf(f->scr_ang);
        bool on_screen = fabsf(dx) < f->scr_hw + 10 && fabsf(dy) < f->scr_hh + 10;
        x = CX + (dx * c + dy * sn) / k;
        y = 262.f + (-dx * sn + dy * c) / k;
        if (!on_screen) {
            // body tickle: a hop, sometimes a shy squirm or a wave
            f->hop_v -= 110;
            float r = frand(f);
            if (r < 0.3f) body_request(f, BA_SHY);
            else if (r < 0.5f) body_request(f, BA_WAVE);
            face_set_emotion(f, EMO_HAPPY, 1.2f);
            f->body.req = f->body.req == BA_JOY ? -1 : f->body.req;  // no full jump for a tickle
            return;
        }
    }
    float ey = f->cur.v[P_EY];
    float sep = f->cur.v[P_SEP] * 0.5f;
    int hit = -1;
    for (int i = 0; i < 2; i++) {
        float ex = CX + (i ? sep : -sep);
        if (fabsf(x - ex) < 70 && fabsf(y - ey) < 90) hit = i;
    }
    if (hit >= 0) {
        f->poke_eye_t[hit] = 0.45f;
        f->hop_v -= 60;
    } else {
        face_set_emotion(f, EMO_JOY, 1.3f);
        if (has_body(f)) {
            f->body.req = -1;  // joy face + hop, keep the body
            f->hop_v -= 80;
        } else {
            face_spawn(f, 1, x, y);
        }
    }
    f->look_tx = clampf((x - CX) * 0.12f, -26, 26);
    f->look_ty = clampf((y - 240) * 0.08f, -18, 18);
    f->next_saccade = 1.5f;
}

static void face_pickup(face_t *f, float x) {
    face_set_emotion(f, EMO_SURPRISED, 1.0f);
    if (x > 0) {  // after a long rest: the start turns into delight (face_update)
        f->pickup_t = 0;
        f->hop_v -= 60;
        f->look_tx = 0;
        f->look_ty = -14;  // looks up at whoever is holding it
        f->next_saccade = 2.5f;
    }
}

static void face_event_action(face_t *f, face_event_t ev, float x) {
    switch (ev) {
    case FEV_PET:  // one swipe or a held finger: a blink and a small hop; joy has to be earned by rubbing (face_rub.c)
        f->hop_v -= 30;
        f->blink_phase = 0;
        f->next_blink = 0;
        break;
    case FEV_SHAKE:
        face_set_emotion(f, EMO_DIZZY, 2.4f);
        break;
    case FEV_NOTIFY:
        f->notify_t = 1.2f;
        face_set_emotion(f, EMO_SURPRISED, 0.9f);
        body_request(f, BA_WAVE);
        break;
    case FEV_NOT_HEARD:
        face_set_emotion(f, EMO_CONFUSED, 2.2f);
        break;
    case FEV_FAIL:
        face_set_emotion(f, EMO_SAD, 2.5f);
        break;
    case FEV_WAKE:
        body_request(f, BA_WAVE);
        f->hop_v -= 120;
        f->blink_phase = 0;
        f->next_blink = 0.3f;
        break;
    case FEV_VOLUME:
        f->volume_level = x;
        f->volume_show = 1.6f;
        f->hop_v -= 50;
        break;
    case FEV_TALK_START:
        f->hop_v -= 50;
        break;
    default:
        break;
    }
}

void face_event(face_t *f, face_event_t ev, float x, float y) {
    f->idle_t = 0;
    if (face_character(f) == CHARACTER_TESS) {
        tess_event(f, ev, x, y);
        return;
    }
    if (ev == FEV_TAP) { face_tap(f, x, y); return; }
    if (ev == FEV_PICKUP) { face_pickup(f, x); return; }
    face_event_action(f, ev, x);
}

static void draw_event_overlay(face_t *f, scene_t *s) {
    if (!f->dark && !f->card_n) event_journal_overlay(&f->journal, s, f->live_active ? 284 : 370);
}

void face_draw(face_t *f, scene_t *s) {
    scene_begin(s, 0x0000);
    if (f->mode == MODE_SETUP && f->pair_code[0]) {
        draw_pair_card(f, s);
        draw_bubble(f, s);
        return;
    }
    if (f->mode == MODE_SETUP && f->setup_text[0][0]) {
        draw_setup_card(f, s);
        return;
    }
    if (f->menu.k >= MENU_SWAP) {  // the character is hidden: not drawn at all
        draw_menu_screen(f, s);
        return;
    }
    if (face_character(f) == CHARACTER_TESS) {
        tess_draw(f, s);
        draw_cron(f, s);
        if (f->offline_k > .01f) {  // no connection: its icon where the cron clock goes, instead of a bubble
            if (f->offline_icon) f->offline_icon_last = f->offline_icon;
            if (f->offline_icon_last) draw_bubble_icon(s, f->offline_icon_last, CRON_X, CRON_Y, BUBBLE_R, f->t, smooth01(f->offline_k));
        }
        draw_card(f, s);
        draw_battery(f, s, false);
        draw_live_status(f, s);
        draw_event_overlay(f, s);
        draw_menu_fade(f, s);
        if (!f->offline_icon || f->bub_icon != f->offline_icon ||
            strcmp(f->bub_text, str(STR_WAITING_FOR_TIME)) == 0) draw_bubble(f, s);
        return;
    }
    face_plush_draw(f, s, face_color(f));
    draw_event_overlay(f, s);
}
