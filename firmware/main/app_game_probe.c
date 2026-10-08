#include "app_game_probe.h"
#include "app_internal.h"
#include "app_lab.h"
#include "input_probe.h"
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

bool app_game_probe(cJSON *j, char *reply, size_t cap) {
    const char *event = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "ev"));
    if (!event) return false;
    if (!strcmp(event, "pointer")) {
        int x, y;
        cJSON *down = cJSON_GetObjectItemCaseSensitive(j, "down");
        bool ok = cJSON_IsBool(down) && panel_coordinate(j, "x", &x) && panel_coordinate(j, "y", &y) &&
            input_probe_sample(cJSON_IsTrue(down), x, y);
        snprintf(reply, cap, "{\"ok\":%s}", ok ? "true" : "false");
        return true;
    }
    if (strcmp(event, "game-state")) return false;
    tess_games_t g; bool preview;
    app_lab_game_snapshot(&g, &preview);
    uint8_t saved;
    if (settings_read_tess_progress(&saved) != ESP_OK) {
        snprintf(reply, cap, "{\"ok\":false,\"error\":\"settings unavailable\"}");
        return true;
    }
    snprintf(reply, cap, "{\"ok\":true,\"preview\":%s,\"progress\":%u,\"saved\":%u,"
             "\"game\":%u,\"phase\":%u,\"tier\":%u,\"step\":%u,\"round\":%lu,"
             "\"available\":%s,\"hit_valid\":%s,\"hit_x\":%.2f,\"hit_y\":%.2f,"
             "\"clock\":%.3f,\"pending\":%u,\"form\":%.3f,\"fold\":%.3f}",
             preview ? "true" : "false", g.progress, saved,
             g.game, g.phase, g.tier, g.step, (unsigned long)g.round,
             g.available ? "true" : "false", g.hit_valid ? "true" : "false", g.hit[0], g.hit[1],
             g.clock, g.pending, g.form, g.fold);
    return true;
}
