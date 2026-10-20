#include "app_game_probe.h"
#include "app_internal.h"
#include "app_lab.h"
#include "input_probe.h"
#include "audio_output_profile.h"
#include "tess_internal.h"
#include "tess_sky_play.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static bool panel_coordinate(cJSON *j, const char *name, int *out) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(j, name);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble != value->valueint ||
        value->valueint < 0 || value->valueint >= 480) return false;
    *out = value->valueint;
    return true;
}

static float s_gravity[2];
static int64_t s_gravity_until;

bool app_game_probe_gravity(float *x, float *y) {
    if (now_ms() >= s_gravity_until) return false;
    *x = s_gravity[0]; *y = s_gravity[1];
    return true;
}
static bool unit(cJSON *j, const char *name, float *out) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(j, name);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || fabs(value->valuedouble) > 1) return false;
    *out = (float)value->valuedouble;
    return true;
}
static bool gravity(cJSON *j) {
    float x, y;
    cJSON *ms = cJSON_GetObjectItemCaseSensitive(j, "ms");
    if (!unit(j, "x", &x) || !unit(j, "y", &y) || !cJSON_IsNumber(ms) || ms->valuedouble < 0 || ms->valuedouble > 10000) return false;
    s_gravity[0] = x; s_gravity[1] = y;
    s_gravity_until = now_ms() + (int64_t)ms->valuedouble;
    return true;
}

static void game_state(char *reply, size_t cap) {
    tess_games_view_t v;
    app_lab_game_snapshot(&v);
    uint8_t saved;
    if (settings_read_tess_progress(&saved) != ESP_OK) {
        snprintf(reply, cap, "{\"ok\":false,\"error\":\"settings unavailable\"}");
        return;
    }
    snprintf(reply, cap, "{\"ok\":true,\"preview\":%s,\"progress\":%u,\"saved\":%u,"
             "\"game\":%u,\"phase\":%u,\"streak\":%u,\"round\":%lu,\"available\":%s,"
             "\"x\":%.1f,\"y\":%.1f,\"waves\":%u,\"bloom\":%.3f,\"tones\":%u,\"spin\":%.2f,\"tilt_x\":%.2f,\"tilt_y\":%.2f,"
             "\"excite\":%.2f,\"annoy\":%.2f,\"fear\":%.2f,\"fond\":%.2f,\"pull\":%.3f,"
             "\"speed\":%.2f,\"squash\":%.3f,\"shaping\":%s,\"burst\":%.2f,\"form\":%.3f}",
             v.preview ? "true" : "false", v.progress, saved,
             v.game, v.phase, v.streak, (unsigned long)v.round, v.available ? "true" : "false",
             v.x, v.y, v.waves, v.bloom, v.tone_kinds, v.spin, v.tilt_x, v.tilt_y, v.excite, v.annoy, v.fear, v.fond, v.pull,
             v.speed, v.squash, v.shaping ? "true" : "false", v.burst, v.form);
}
// Skies diagnostics use real, expiring finger/tilt input; they never move level geometry to solve it.
static uint32_t s_sky_saved;       // the player's own progress, taken before the first forced sky
static bool s_sky_kept;
static atomic_int s_sky_request;   // elements << 8 | skill; 0 none (the face's own task starts it: this task's stack is small)
static bool sky_start(cJSON *j) {
    cJSON *el = cJSON_GetObjectItemCaseSensitive(j, "elements"), *sk = cJSON_GetObjectItemCaseSensitive(j, "skill");
    if (!cJSON_IsNumber(el) || el->valueint < 1 || el->valueint > SKY_LESSONS || !cJSON_IsNumber(sk) || sk->valueint < 0 || sk->valueint > 255)
        return false;
    if (!s_sky_kept) {
        bool settled;
        face_lock();
        s_sky_saved = tess_games_sky_progress(&g_face, &settled);
        face_unlock();
        s_sky_kept = true;
    }
    atomic_store(&s_sky_request, el->valueint << 8 | sk->valueint);
    return true;
}
void app_game_probe_poll(face_t *f) {
    int request = KUBIK_CHARACTER == CHARACTER_TESS ? atomic_exchange(&s_sky_request, 0) : 0;
    if (!request) return;
    f->tess_games.sky_elements = (uint8_t)(request >> 8);
    f->tess_games.sky_skill = (uint8_t)request;
    tess_sky_start(f);
}
static bool sky_restore(void) {
    if (!s_sky_kept) return true;
    face_lock();
    bool idle = !g_face.tess_games.sky;
    if (idle) tess_games_restore_sky(&g_face, s_sky_saved);
    face_unlock();
    s_sky_kept = !idle;
    return idle;
}
static bool sky_solve(void) {
    sky_force_t force = {0};
    face_lock();
    const tess_sky_t *k = g_face.tess_games.sky;
    bool playing = k && k->phase == SKY_PLAY;
    if (playing) {
        sky_solution(&k->w, &force);
        float scale = k->w.kind == SKY_POUR ? 1.f : SKY_TILT_SCALE;
        force.gx /= scale; force.gy /= scale;
    }
    face_unlock();
    if (!playing) return false;
    s_gravity[0] = force.gx; s_gravity[1] = force.gy;
    s_gravity_until = now_ms() + 350;
    return input_probe_sample(force.count > 0, force.count ? (int)force.touch[0].x : 0, force.count ? (int)force.touch[0].y : 0);
}
static void sky_state(char *reply, size_t cap) {
    face_lock();
    const tess_games_t *g = &g_face.tess_games;
    const tess_sky_t *k = g->sky;
    bool settled;
    unsigned long packed = tess_games_sky_progress(&g_face, &settled);
    int n = snprintf(reply, cap, "{\"ok\":true,\"active\":%s,\"elements\":%u,\"skill\":%u,\"solved\":%u,\"root\":%u,\"invite\":%.1f,\"call_x\":%.1f,\"call_y\":%.1f,"
                     "\"call_body_x\":%.1f,\"call_body_y\":%.1f,\"packed\":%lu,\"saved\":%lu",
                     k ? "true" : "false", g->sky_elements, g->sky_skill, g->sky_solved, g->sky_root, g->sky_invite_t, g->sky_call[0], g->sky_call[1],
                     g->vertex_px[g->sky_call_vertex][0], g->vertex_px[g->sky_call_vertex][1], packed, (unsigned long)g_settings.sky_progress);
    if (k && n > 0 && (size_t)n < cap) {
        sky_force_t solution; sky_solution(&k->w, &solution);
        int full = 0, touches = 0;
        for (int i = 0; i < k->w.count_stars; i++) full += k->w.star[i].fill >= SKY_FULL;
        for (int i = 0; i < SKY_TOUCHES; i++) touches += k->touch[i].down;
        snprintf(reply + n, cap - (size_t)n, ",\"phase\":%u,\"kind\":%u,\"chart\":%u,\"turn\":%u,\"emitter\":%u,\"channels\":%u,\"stars\":%u,\"full\":%d,\"holes\":%u,\"touches\":%d,\"finger\":%s,"
                 "\"gx\":%.2f,\"gy\":%.2f,\"hold\":%.2f,\"t\":%.1f,\"here\":%u,\"sx\":%.0f,\"sy\":%.0f,\"melody\":%u,\"sung\":%u}",
                 k->phase, k->w.kind, k->w.chart, k->w.turn, k->w.emitter, k->w.channels, k->w.count_stars, full, k->w.count_holes, touches, solution.count ? "true" : "false",
                 solution.gx, solution.gy, k->hold, k->w.t, k->solved_here,
                 k->w.source.x, k->w.source.y, k->melody.n, k->melody_next);
    } else if (n > 0 && (size_t)n < cap) snprintf(reply + n, cap - (size_t)n, "}");
    face_unlock();
}
static bool pointer(cJSON *j) {
    int x, y;
    cJSON *down = cJSON_GetObjectItemCaseSensitive(j, "down");
    return cJSON_IsBool(down) && panel_coordinate(j, "x", &x) && panel_coordinate(j, "y", &y) &&
        input_probe_sample(cJSON_IsTrue(down), x, y);
}
static bool sky_event(cJSON *j, const char *event, char *reply, size_t cap) {
    if (!strcmp(event, "sky-state")) {
        if (KUBIK_CHARACTER == CHARACTER_TESS) sky_state(reply, cap);
        else snprintf(reply, cap, "{\"ok\":true,\"active\":false}");
        return true;
    }
    if (strcmp(event, "sky-start") && strcmp(event, "sky-solve") && strcmp(event, "sky-restore")) return false;
    bool ok = KUBIK_CHARACTER == CHARACTER_TESS &&   // (Plush has no Skies, and links none of it)
        (!strcmp(event, "sky-start") ? sky_start(j) : !strcmp(event, "sky-solve") ? sky_solve() : sky_restore());
    snprintf(reply, cap, "{\"ok\":%s}", ok ? "true" : "false");
    return true;
}
bool app_game_probe(cJSON *j, char *reply, size_t cap) {
    const char *event = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "ev"));
    if (!event) return false;
    if (!strcmp(event, "audio-stats")) {
        audio_output_profile_t p = audio_output_profile_read(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "reset")));
        snprintf(reply, cap, "{\"ok\":true,\"frames\":%lu,\"audible_frames\":%lu,\"mix_max_us\":%lu,"
                 "\"feed_max_us\":%lu,\"late_feeds\":%lu,\"write_errors\":%lu}",
                 (unsigned long)p.frames, (unsigned long)p.audible_frames, (unsigned long)p.mix_max_us,
                 (unsigned long)p.feed_max_us, (unsigned long)p.late_feeds, (unsigned long)p.write_errors);
        return true;
    }
    if (!strcmp(event, "pointer") || !strcmp(event, "gravity")) {
        bool ok = !strcmp(event, "pointer") ? pointer(j) : gravity(j);
        snprintf(reply, cap, "{\"ok\":%s}", ok ? "true" : "false");
        return true;
    }
    if (sky_event(j, event, reply, cap)) return true;
    if (strcmp(event, "game-state")) return false;
    game_state(reply, cap);
    return true;
}
