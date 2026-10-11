#include "app_game_probe.h"
#include "app_internal.h"
#include "app_lab.h"
#include "input_probe.h"
#include "audio_output_profile.h"
#include <math.h>
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
    tess_games_t g; bool preview;
    app_lab_game_snapshot(&g, &preview);
    uint8_t saved;
    if (settings_read_tess_progress(&saved) != ESP_OK) {
        snprintf(reply, cap, "{\"ok\":false,\"error\":\"settings unavailable\"}");
        return;
    }
    unsigned waves = 0;
    for (int i = 0; i < TESS_WAVES; i++) waves += g.wave[i].amp != 0;
    snprintf(reply, cap, "{\"ok\":true,\"preview\":%s,\"progress\":%u,\"saved\":%u,"
             "\"game\":%u,\"phase\":%u,\"streak\":%u,\"round\":%lu,\"available\":%s,"
             "\"x\":%.1f,\"y\":%.1f,\"waves\":%u,\"bloom\":%.3f,\"tones\":%u,\"spin\":%.2f,\"tilt_x\":%.2f,\"tilt_y\":%.2f,"
             "\"excite\":%.2f,\"annoy\":%.2f,\"fear\":%.2f,\"fond\":%.2f,\"pull\":%.3f,"
             "\"speed\":%.2f,\"squash\":%.3f,\"self\":%u,\"shaping\":%s,\"burst\":%.2f,\"form\":%.3f}",
             preview ? "true" : "false", g.progress, saved,
             g.game, g.phase, g.streak, (unsigned long)g.round, g.available ? "true" : "false",
             g.hit[0], g.hit[1], waves, g.bloom, g.tone_kinds, g.spin, g.tilt[0], g.tilt[1], g.excite, g.annoy, g.fear, g.fond, g.pull_k,
             hypotf(g.fly_v[0], g.fly_v[1]), g.squash_k, g.self_act, g.shaping ? "true" : "false", g.gather_clock, g.form);
}
static bool pointer(cJSON *j) {
    int x, y;
    cJSON *down = cJSON_GetObjectItemCaseSensitive(j, "down");
    return cJSON_IsBool(down) && panel_coordinate(j, "x", &x) && panel_coordinate(j, "y", &y) &&
        input_probe_sample(cJSON_IsTrue(down), x, y);
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
    if (strcmp(event, "game-state")) return false;
    game_state(reply, cap);
    return true;
}
