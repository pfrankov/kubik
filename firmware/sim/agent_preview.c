// Exact native renderer snapshots for touch-layout review; no device or provider calls.
#include "agent_menu.h"
#include "face_agent.h"
#include "render.h"
#include "face.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PANEL_W = R_W * R_SCALE, PANEL_H = R_H * R_SCALE };
static uint16_t canvas[R_H][R_W], panel[PANEL_H][PANEL_W];
static void push(int x0, int y0, int x1, int y1, uint16_t *pixels, void *ctx) {
    (void)ctx;
    for (int y = y0; y < y1; y++) memcpy(canvas[y] + x0, pixels + (y - y0) * (x1 - x0), (x1 - x0) * 2);
}
static void read_row(void *ctx, int y, uint16_t *row, int xa, int xb) {
    (void)ctx; (void)xa; (void)xb;
    memcpy(row, canvas[y], sizeof canvas[y]);
}
static void fixture(agent_menu_t *m) {
    agent_menu_reset(m);
    agent_menu_show(m, true);
    m->options_loaded = m->capabilities_known = m->stt_available = m->tts_available = true;
    m->total = 19; m->count = 4;
    const char *ids[] = {"openai/gpt-6.1-sol", "openai/gpt-6-luna", "anthropic/claude-sonnet-4.6", "openrouter/very-long-provider/model-name"};
    const char *labels[] = {"GPT-6.1 Sol", "GPT-6 Luna", "Claude Sonnet 4.6", "Claude Sonnet 4.6 Extended Thinking"};
    for (unsigned i = 0; i < 4; i++) {
        snprintf(m->models[i].id, sizeof m->models[i].id, "%s", ids[i]);
        snprintf(m->models[i].label, sizeof m->models[i].label, "%s", labels[i]);
    }
    strcpy(m->model, ids[0]);
    strcpy(m->stt_provider, "openai"); strcpy(m->stt_model, "gpt-live-transcribe");
    strcpy(m->tts_provider, "openai"); strcpy(m->tts_model, "gpt-4o-mini-tts");
}
static void voice_catalog(agent_menu_t *menu, const char *flag) {
    if (strstr(flag, "input") || strstr(flag, "output")) {
        menu->target = strstr(flag, "input") ? AGENT_TARGET_STT : AGENT_TARGET_TTS;
        menu->count = menu->total = 1;
        const char *model = menu->target == AGENT_TARGET_STT ? menu->stt_model : menu->tts_model;
        snprintf(menu->models[0].id, sizeof menu->models[0].id, "openai/%s", model);
        strcpy(menu->models[0].label, model); strcpy(menu->selected_model, menu->models[0].id);
    }
    if (strstr(flag, "input-missing")) menu->count = menu->total = 0;
    if (strstr(flag, "picker")) {
        menu->target = AGENT_TARGET_VOICE; menu->count = menu->total = 3;
        const char *ids[] = {"gpt-realtime-2.1", "gpt-realtime-2.1-mini", "gpt-realtime-2"};
        for (unsigned i = 0; i < 3; ++i) {
            snprintf(menu->models[i].id, sizeof menu->models[i].id, "openai/%s", ids[i]);
            if (menu->voice_mode == VOICE_LIVE) {
                menu->count = menu->total = 1;
                strcpy(menu->models[i].id, "openai/gpt-live-1");
                strcpy(menu->models[i].label, "gpt-live-1");
            } else strcpy(menu->models[i].label, ids[i]);
        }
        strcpy(menu->selected_model, menu->models[0].id);
    }
}
static void mode_catalog(agent_menu_t *menu, const char *flag) {
    if (strstr(flag, "modes")) {
        menu->target = AGENT_TARGET_MODE; menu->count = menu->total = 3;
        const char *ids[] = {"classic", "realtime", "live"};
        const char *labels[] = {"STT", "Realtime", "GPT Live"};
        for (unsigned i = 0; i < 3; ++i) {
            strcpy(menu->models[i].id, ids[i]); strcpy(menu->models[i].label, labels[i]);
            menu->models[i].disabled = strstr(flag, "missing") && i > 0;
        }
        strcpy(menu->selected_model, ids[menu->voice_mode]);
    }
}
static void flags(agent_menu_t *menu, const char *flag) {
    if (strstr(flag, "realtime")) menu->voice_mode = VOICE_REALTIME;
    if (strstr(flag, "live")) menu->voice_mode = VOICE_LIVE;
    if (menu->voice_mode != VOICE_CLASSIC) {
        const char *model = menu->voice_mode == VOICE_LIVE ? "gpt-live-1" : "gpt-realtime-2.1";
        strcpy(menu->stt_model, model); strcpy(menu->tts_model, model);
    }
    voice_catalog(menu, flag);
    mode_catalog(menu, flag);
    if (!strcmp(flag, "error")) { strcpy(menu->error, "timeout"); menu->retry_kind = AGENT_REQUEST_SELECT; }
    if (!strcmp(flag, "offline")) agent_menu_set_online(menu, false);
    if (strstr(flag, "missing")) menu->stt_available = menu->tts_available = false;
    if (!strcmp(flag, "quiet")) menu->volume = 19;
    if (!strcmp(flag, "entering")) menu->guide_elapsed = .07f;
    if (!strcmp(flag, "pending")) {
        menu->request_pending = true; menu->waiting_kind = AGENT_REQUEST_SELECT;
        strcpy(menu->retry_id, menu->models[1].id);
    }
}
static void settings(scene_t *scene, int volume, bool sound) {
    face_t face = {0}; face.menu.open = true; face.menu.k = 1;
    face.menu.pressed = -1; face.menu.pressed_t = 1;
    face.menu.sound = sound; face.menu.ui_volume = 40;
    face.menu.volume = volume; face.menu.brightness = 80;
    face.menu.txt[MT_WIFI] = "Wi-Fi"; face.menu.txt[MT_POWER] = "Power off"; face.menu.txt[MT_RESET] = "Reset";
    face.battery_present = true; face.battery_pct = 85; face.charging = true; face.usb_power = true;
    void draw_menu(face_t *, scene_t *);
    draw_menu(&face, scene);
}
static void live(scene_t *scene, const char *state) {
    face_t face; face_init(&face); face_set_character(&face, CHARACTER_TESS);
    face_set_mode(&face, MODE_IDLE);
    for (int i = 0; i < 90; i++) face_update(&face, 1.f / 30);
    face.live_active = true; face.live_ready = strcmp(state, "connecting") != 0;
    face.live_mic = face.live_ready;
    face_set_mode(&face, !strncmp(state, "mic", 3) ? MODE_LISTENING : !strncmp(state, "speaking", 8) ? MODE_SPEAKING : MODE_THINKING);
    face.mic_level = strstr(state, "quiet") ? 0 : strstr(state, "low") ? .15f : .7f;
    face.spk_level = .7f;
    for (int i = 0; i < 90; i++) face_update(&face, 1.f / 30);
    if (strstr(state, "card")) {
        face_card(&face, "Your agent is connected.\nThis reply is shown as text.\nLive keeps listening after\nits response has finished.\nPress KEY when you want\nto end the conversation.\nThis is a second page.");
        face.card_hold = true;
        for (int i = 0; i < 15; i++) face_update(&face, 1.f / 30);
    }
    if (strstr(state, "charging")) {
        face.battery_present = face.charging = face.usb_power = true; face.battery_pct = 60;
    }
    face_draw(&face, scene);
}
static void clock_preview(scene_t *scene, const char *variant) {
        face_t face; face_init(&face); face_set_character(&face, CHARACTER_TESS);
        face_set_mode(&face, MODE_IDLE);
        for (int i = 0; i < 120; i++) face_update(&face, 1.f / 30);
        face.cron_running = 1; face.cron_k = 1;
        if (!strcmp(variant, "low")) face_set_power(&face, true, 12, false, false);
        if (!strcmp(variant, "due")) { face.cron_running = 0; face.cron_due = face.t + 720; }
        face_draw(&face, scene);
}

static void journal_preview(scene_t *scene, const char *variant) {
    face_t f; face_init(&f); face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, !strcmp(variant, "live") ? MODE_LISTENING : MODE_IDLE);
    for (int i = 0; i < 120; i++) face_update(&f, 1.f / 30);
    event_journal_t *j = &f.journal;
    if (strcmp(variant, "empty")) {
        event_journal_add(j, 123, JOURNAL_LINK, "Agent connected", "Ready / connection greeting");
        event_journal_add(j, 125, JOURNAL_ACTIVITY, "Here: tool / elsewhere: coding", "Waiting pose / activity indicator");
        event_journal_add(j, 126, JOURNAL_REPLY, "The result is ready. This longer example lets you read the message excerpt in detail without sending a request to an agent.", "Reply / text card");
    }
    j->overlay = true;
    if (!strcmp(variant, "overlay") || !strcmp(variant, "live")) {
        f.live_active = f.live_ready = f.live_mic = !strcmp(variant, "live");
        f.cron_running = 1; f.cron_k = 1; face_set_power(&f, true, 10, true, true);
    } else { f.menu.open = true; f.menu.k = 1; event_journal_open(j); j->detail = !strcmp(variant, "detail"); }
    face_draw(&f, scene);
}
static void draw_preview_scene(scene_t *scene, agent_menu_t *menu, int argc, char **argv) {
    const char *kind = argc > 1 ? argv[1] : "";
    if (!strcmp(kind, "settings")) settings(scene, argc > 2 ? atoi(argv[2]) : 70, argc > 3);
    else if (!strcmp(kind, "live")) live(scene, argc > 2 ? argv[2] : "mic");
    else if (!strcmp(kind, "journal")) journal_preview(scene, argc > 2 ? argv[2] : "list");
    else if (!strcmp(kind, "clock")) {
        clock_preview(scene, argc > 2 ? argv[2] : "");
    }
    else face_agent_draw(scene, menu, !(argc > 3 && strstr(argv[3], "plush")));
}

int main(int argc, char **argv) {
    agent_menu_t menu; fixture(&menu);
    menu.view = argc > 1 ? (agent_view_t)atoi(argv[1]) : AGENT_VIEW_OVERVIEW;
    menu.half = argc > 2 ? (uint8_t)atoi(argv[2]) : 0;
    menu.guide_step = menu.half; menu.guide_elapsed = 1; menu.volume = 70;
    flags(&menu, argc > 3 ? argv[3] : "");
    scene_t scene; scene_begin(&scene, 0);
    draw_preview_scene(&scene, &menu, argc, argv);
    uint16_t a[R_W * R_H], b[R_W * R_H], *buffers[] = {a, b};
    render_state_t state = {0};
    render_frame(&state, &scene, buffers, 2, false, push, NULL);
    render_output_t output; render_output_begin(&output, read_row, NULL);
    render_expand_2x(&output, &panel[0][0], 0, 0, PANEL_W, PANEL_H, false);
    render_edge_overlay(&scene, &panel[0][0], 0, 0, PANEL_W, PANEL_H, false);
    render_text_overlay(&scene, &panel[0][0], 0, 0, PANEL_W, PANEL_H, false);
    for (int y = 0; y < PANEL_H; y++) for (int x = 0; x < PANEL_W; x++) {
        unsigned p = panel[y][x];
        unsigned char rgb[] = {((p >> 11) & 31) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31};
        fwrite(rgb, 1, 3, stdout);
    }
    return 0;
}
