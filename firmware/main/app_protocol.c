#include "frame_diagnostics.h"
#include "app_emoji.h"
#include "app_config_info.h"
#include "app_lab.h"
#include "app_models.h"
#include "app_status.h"
#include "app_journal.h"
#include "app_game_probe.h"
#include "app_internal.h"
#include "setup_request.h"
#include "printer_protocol.h"
#include "printer_preview.h"
#include "link_usb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agent_protocol.h"
#include "muse_store.h"
#include "audio.h"
#include "board.h"
#include "cJSON.h"
#include "devkey.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "heap_probe.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "esp_pm.h"
#include "version.h"
#include "wifi.h"
static const char *TAG = "app";
static const char *const k_states[] = {"idle", "listening", "transcribing", "thinking", "speaking"};
static int act_index(cJSON *j, const char *key) {
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(j, key));
    for (int i = 1; v && i < ACT_COUNT; i++)
        if (!strcmp(v, app_activity_names[i])) return i;
    return v && v[0] ? ACT_TOOL : ACT_NONE;  // a category from a newer server: still "busy"
}
static int protocol_integer(cJSON *object, const char *key, int min, int max) {
    cJSON *number = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(number) || number->valuedouble < min || number->valuedouble > max ||
        number->valuedouble != (double)number->valueint) return -1;
    return number->valueint;
}
static int protocol_volume(cJSON *object, const char *key) {
    return protocol_integer(object, key, 0, 100);
}
static void handle_agent_options(cJSON *j) {
    agent_menu_reply_t *reply = calloc(1, sizeof(*reply));
    if (reply) {
        if (agent_protocol_parse_options(j, reply)) app_agent_receive_reply(reply);
        free(reply);
    }
}
static const char *const k_errors[] = {"stt_empty", "stt_failed", "agent_failed", "voice_failed", "busy", "unauthorized"};
// Wire events carry their route epoch through the app queue.
static void post_remote(app_ev_type_t type, int a, int b) {
    app_post_in_session(type, a, b, link_session());
}
static void handle_welcome_json(cJSON *j) {
    post_remote(EV_SRV_WELCOME, protocol_volume(j, "volume"),
             cJSON_IsTrue(cJSON_GetObjectItem(j, "progress")));
}
static void handle_state_json(cJSON *j) {
    const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(j, "s"));
    for (int i = 0; state && i < 5; i++)
        if (!strcmp(state, k_states[i])) post_remote(EV_SRV_STATE, i, 0);
}
static void handle_activity_json(cJSON *j) {
    post_remote(EV_SRV_ACTIVITY, act_index(j, "own"), act_index(j, "other"));
}
static void handle_emotion_json(cJSON *j) {
    const char *emotion = cJSON_GetStringValue(cJSON_GetObjectItem(j, "e"));
    cJSON *duration = cJSON_GetObjectItem(j, "ms");
    if (emotion)
        post_remote(EV_SRV_EMOTION, face_emotion_from_name(emotion), cJSON_IsNumber(duration) ? duration->valueint : 0);
}
static void handle_speak_json(cJSON *j) {
    int gen = app_voice_parse_generation(j);
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(j, "kind"));
    if (gen < 0) return;
    uint32_t session = link_session();
    if (!app_speech_begin(gen, session)) return;
    ESP_LOGI(TAG, "speech begin gen=%d via=%s", gen, link_via());
    hp_mark("speech begin");
    app_post_in_session(EV_SRV_SPEAK, gen, kind && !strcmp(kind, "notify"), session);
}
static void handle_speak_end_json(cJSON *j) {
    int gen = app_voice_parse_generation(j);
    if (gen >= 0) app_post_in_session(EV_SRV_SPEAK_END, gen, 0, link_session());
}
static void handle_error_json(cJSON *j) {
    const char *error = cJSON_GetStringValue(cJSON_GetObjectItem(j, "code"));
    int code = ERR_OTHER;
    for (int i = 0; error && i < 6; i++)
        if (!strcmp(error, k_errors[i])) code = i;
    post_remote(EV_SRV_ERROR, code, 0);
}
static void handle_text_json(cJSON *j) {
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(j, "text"));
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(j, "kind"));
    if (!text) return;
    portENTER_CRITICAL(&s_text_mux);
    snprintf(s_text_rx, sizeof s_text_rx, "%s", text);
    cJSON *receipt = cJSON_GetObjectItem(j, "receipt");
    s_text_receipt = cJSON_IsNumber(receipt) && receipt->valuedouble > 0 &&
        receipt->valuedouble <= UINT32_MAX && receipt->valuedouble == (double)(uint32_t)receipt->valuedouble
        ? (uint32_t)receipt->valuedouble : 0;
    s_text_session = link_session();
    s_text_notify = kind && !strcmp(kind, "notify");
    uint32_t revision = ++s_text_revision;
    portEXIT_CRITICAL(&s_text_mux);
    post_remote(EV_SRV_TEXT, (int32_t)revision, kind && !strcmp(kind, "notify"));
}
static void handle_cron_json(cJSON *j) {
    cJSON *running = cJSON_GetObjectItem(j, "running"), *next = cJSON_GetObjectItem(j, "next");
    post_remote(EV_SRV_CRON, cJSON_IsNumber(running) ? running->valueint : 0,
             cJSON_IsNumber(next) ? next->valueint : -1);
}
static void handle_set_json(cJSON *j) {
    int setting_volume = protocol_volume(j, "volume");
    post_remote(EV_SRV_SET, setting_volume, protocol_integer(j, "brightness", 10, 255));
}
static void dispatch_json(cJSON *j, const char *type) {
    if (app_voice_receive(j, type)) return;
    if (!strcmp(type, "welcome")) handle_welcome_json(j);
    else if (!strcmp(type, "agent_options")) handle_agent_options(j);
    else if (!strcmp(type, "state")) handle_state_json(j);
    else if (!strcmp(type, "activity")) handle_activity_json(j);
    else if (!strcmp(type, "emotion")) handle_emotion_json(j);
    else if (!strcmp(type, "speak")) handle_speak_json(j);
    else if (!strcmp(type, "speak_end")) handle_speak_end_json(j);
    else if (!strcmp(type, "error")) handle_error_json(j);
    else if (!strcmp(type, "text")) handle_text_json(j);
    else if (!strcmp(type, "cron")) handle_cron_json(j);
    else if (!strcmp(type, "set")) handle_set_json(j);
}
static void on_json(const char *json, size_t len) {
    const char *end; cJSON *j = cJSON_ParseWithLengthOpts(json, len, &end, false);
    if (!j) { ESP_LOGW(TAG, "JSON rejected (%u bytes)", (unsigned)len); return; }
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(j, "t"));
    if (!app_emoji_receive(j, json, len, (size_t)(end - json)) && !printer_protocol_receive(j, json, len, (size_t)(end - json), link_session()) && type &&
        (!strcmp(type, "agent_options") ? !agent_protocol_has_nul_escape(json, len) : true)) dispatch_json(j, type);
    cJSON_Delete(j);
}
static void on_audio(uint8_t kind, uint8_t tag, const uint8_t *pcm, size_t len) {
    if (kind == 0x03) app_speech_write(tag, link_session(), pcm, len);
    else if (kind == PRINTER_IMAGE_BINARY_KIND) printer_protocol_image_receive(tag, pcm, len, link_session());
    else if (kind == PRINTER_PREVIEW_KIND) printer_preview_receive(tag, pcm, len, link_session());
}
static void on_pair(const char *code, bool telegram) {
    portENTER_CRITICAL(&s_pair_mux);
    snprintf(s_pair_rx, sizeof s_pair_rx, "%s", code);
    s_pair_rx_telegram = telegram;
    portEXIT_CRITICAL(&s_pair_mux);
    app_post(EV_PAIR, 0, 0);
}
static void on_link(bool up) { if (!up) printer_preview_clear(0); hp_mark(up ? "link up" : "link down"); app_post(up ? EV_LINK_UP : EV_LINK_DOWN, 0, 0); }
static bool config_connection_is_valid(cJSON *url, cJSON *ssid, cJSON *pass) {
    return (!url || cJSON_IsString(url)) && (!ssid || cJSON_IsString(ssid)) && (!pass || cJSON_IsString(pass));
}
static const char *config_connection_error(esp_err_t err) {
    return err == ESP_ERR_INVALID_ARG ? "invalid connection settings" : "could not save connection settings";
}
static bool config_save_connection(cJSON *j, bool *connection_change, char *reply, size_t cap) {
    cJSON *url = cJSON_GetObjectItem(j, "url");
    cJSON *ssid = cJSON_GetObjectItem(j, "ssid");
    cJSON *pass = cJSON_GetObjectItem(j, "pass");
    *connection_change = url || ssid || pass || cJSON_GetObjectItem(j, "agent");
    if (!*connection_change) return true;
    if (!config_connection_is_valid(url, ssid, pass)) {
        snprintf(reply, cap, "{\"ok\":false,\"error\":\"invalid connection settings\"}");
        return false;
    }
    esp_err_t err = setup_host_connection_save(j, ssid ? ssid->valuestring : g_settings.wifi_ssid,
                                             pass ? pass->valuestring : g_settings.wifi_pass,
                                             url ? url->valuestring : g_settings.server_url);
    if (err == ESP_OK) return true;
    snprintf(reply, cap, "{\"ok\":false,\"error\":\"%s\"}", config_connection_error(err));
    return false;
}
static void config_set_values(cJSON *j, char *reply, size_t cap) {
    bool connection_change;
    if (!config_save_connection(j, &connection_change, reply, cap)) return;
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
    if (name) snprintf(g_settings.name, sizeof g_settings.name, "%s", name);
    cJSON *volume = cJSON_GetObjectItem(j, "volume");
    if (cJSON_IsNumber(volume)) {
        g_settings.volume = volume->valueint < 0 ? 0 : volume->valueint > 100 ? 100 : volume->valueint;
        audio_set_volume(g_settings.volume);
        device_state_volume_report(true);
    }
    int ui_volume = protocol_volume(j, "ui_volume");
    if (ui_volume >= 0) {
        g_settings.ui_volume = ui_volume;
        audio_set_ui_volume(ui_volume);
    }
    cJSON *brightness = cJSON_GetObjectItem(j, "brightness");
    if (cJSON_IsNumber(brightness) && brightness->valueint >= 10 && brightness->valueint <= 255) {
        g_settings.brightness = brightness->valueint;
        if (!power_is_asleep(s_power)) disp_brightness_fade(power_brightness(s_power, brightness->valueint), 300);
    }
    settings_save();
    snprintf(reply, cap, "{\"ok\":true,\"rebooting\":%s}", connection_change ? "true" : "false");
    if (connection_change) app_post(EV_BOOT_LONG, 1, 0);
}
static void config_set(cJSON *j, char *reply, size_t cap) {
    app_wake_configuration_busy(true);
    config_set_values(j, reply, cap);
    app_wake_configuration_busy(false);
}
// Hooks that last for a `ms` given (or their default): the view turns about a changing axis (as in `sim rotate`),
// a finger rubs the character (as in `sim rub vigorous`), or the Wi-Fi session stops (routing test; nothing is saved).
static bool sim_timed_hook(cJSON *j, const char *event) {
    cJSON *ms = cJSON_GetObjectItem(j, "ms");
    int64_t given = cJSON_IsNumber(ms) && ms->valueint > 0 ? ms->valueint : 0;
    if (!strcmp(event, "spin")) s_debug_spin_until = now_ms() + (given ? given : 8000);
    else if (!strcmp(event, "rub")) s_debug_rub_until = now_ms() + (given ? given : 9000);
    else if (!strcmp(event, "wifi")) link_pause_wifi(given ? (uint32_t)given : 30000);
    else if (!strcmp(event, "power-network") && link_usb_host_present())
        s_debug_network_until = now_ms() + (given > 120000 ? 120000 : given ? given : 120000);
    else return false;
    return true;
}
static void sim_mood_hook(cJSON *j) {  // Native Tess mood diagnostics.
    if (KUBIK_CHARACTER != CHARACTER_TESS) return;
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "mood"));
    cJSON *level = cJSON_GetObjectItem(j, "level");
    for (int mood = 0; name && mood < MOOD_COUNT; mood++) {
        if (strcmp(name, mood_name((mood_t)mood))) continue;
        face_lock();
        face_force_mood(&g_face, (mood_t)mood, cJSON_IsNumber(level) ? (float)level->valuedouble : 1.f);
        face_unlock();
    }
}
// Screen, pairing and display diagnostic hooks (tools/test-device.mjs).
static bool sim_screen_hook(cJSON *j, const char *event) {
    if (!strcmp(event, "mode")) {
        cJSON *mode = cJSON_GetObjectItem(j, "mode"), *ms = cJSON_GetObjectItem(j, "ms");
        s_debug_mode = cJSON_IsNumber(mode) ? mode->valueint : MODE_IDLE;
        s_debug_until = now_ms() + (cJSON_IsNumber(ms) ? ms->valueint : 15000);
        atomic_store(&g_mode_report, true);
    } else if (!strcmp(event, "setup")) {
        app_post(EV_SETUP_START, SETUP_BY_USER, 0);
    } else if (!strcmp(event, "mood")) {
        sim_mood_hook(j);
    } else {
        return false;
    }
    return true;
}
static bool sim_drag_hook(cJSON *j, const char *event) {
    if (strcmp(event, "drag")) return false;
    cJSON *x = cJSON_GetObjectItem(j, "x"), *y = cJSON_GetObjectItem(j, "y");
    if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) || x->valueint < 0 || x->valueint >= UI_W ||
        y->valueint < 0 || y->valueint >= UI_H) return false;
    bool first = cJSON_IsTrue(cJSON_GetObjectItem(j, "first"));
    bool last = cJSON_IsTrue(cJSON_GetObjectItem(j, "last"));
    app_post(EV_DRAG, x->valueint | (first ? 1 << 16 : 0) | (last ? 1 << 17 : 0), y->valueint);
    return true;
}
static bool sim_hook(cJSON *j, const char *event) {
    if (sim_drag_hook(j, event)) return true;
    if (sim_screen_hook(j, event) || sim_timed_hook(j, event)) return true;
    if (strcmp(event, "pair")) return false;
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(j, "code"));  // shown for 20 s unless sent again; an empty code closes it
    if (code && !code[0]) s_pair_code[0] = 0;
    else on_pair(code ? code : "TEST2345", false);
    return true;
}
static void config_sim(cJSON *j, char *reply, size_t cap) {
    const char *event = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ev"));
    if (event && !strcmp(event, "stats")) {
        frame_diagnostics_reply(reply, cap); return;
    }
    if (app_game_probe(j, reply, cap)) return;
    static const struct {
        const char *name;
        app_ev_type_t ev;
    } events[] = {{"ptt_down", EV_PTT_DOWN}, {"ptt_up", EV_PTT_UP}, {"tap", EV_TAP},
                  {"hold", EV_MENU_HOLD}, {"pet", EV_PET}, {"shake", EV_SHAKE}, {"pickup", EV_PICKUP},
                  {"boot", EV_BOOT_SHORT}, {"pwr", EV_PWR_SHORT}, {"menu", EV_BOOT_LONG},
                  {"pwr_long", EV_PWR_LONG}};
    cJSON *x = cJSON_GetObjectItem(j, "x"), *y = cJSON_GetObjectItem(j, "y");
    bool found = event && sim_hook(j, event);
    for (size_t i = 0; event && i < sizeof(events) / sizeof(events[0]); i++) {
        if (!strcmp(event, events[i].name)) {
            app_post(events[i].ev, cJSON_IsNumber(x) ? x->valueint : 240, cJSON_IsNumber(y) ? y->valueint : UI_CY);
            found = true;
        }
    }
    snprintf(reply, cap, "{\"ok\":%s}", found ? "true" : "false");
}
static void config_card(cJSON *j, char *reply, size_t cap) {
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(j, "text"));
    portENTER_CRITICAL(&s_text_mux);
    s_text_receipt = s_text_session = 0;
    s_text_notify = false;
    snprintf(s_text_rx, sizeof s_text_rx, "%s", text ? text : "");
    uint32_t revision = ++s_text_revision;
    portEXIT_CRITICAL(&s_text_mux);
    app_post(EV_SRV_TEXT, (int32_t)revision, 0);
    snprintf(reply, cap, "{\"ok\":true}");
}
static void config_lcd(char *reply, size_t cap) {
    disp_reinit();
    snprintf(reply, cap, "{\"ok\":true}");
}
static void config_reboot(char *reply, size_t cap) {
    snprintf(reply, cap, "{\"ok\":true}");
    app_post(EV_BOOT_LONG, 1, 0);
}
static void config_reset(char *reply, size_t cap) {
    esp_err_t err = factory_reset();
    if (err == ESP_OK) snprintf(reply, cap, "{\"ok\":true}");
    else snprintf(reply, cap, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
}
static void config_info(char *reply, size_t cap) {
    status_json_t info;
    status_json_begin(&info, reply, cap);
    app_config_info_fields(&info);
    if (!status_json_end(&info)) snprintf(reply, cap, "{\"ok\":false,\"error\":\"status unavailable\"}");
}
static void config_muse(cJSON *j, char *reply, size_t cap) {
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "sdk_token"));
    esp_err_t err = talk_is_listening(s_talk) || app_voice_live_active() || s_srv != SS_IDLE
        ? ESP_ERR_INVALID_STATE : muse_store_begin(token);
    snprintf(reply, cap, "{\"ok\":%s,\"rebooting\":%s}", err == ESP_OK ? "true" : "false", err == ESP_OK ? "true" : "false");
    if (token) muse_store_wipe((void *)token, strlen(token));
    if (err == ESP_OK) app_post(EV_BOOT_LONG, 1, 0);
}
static void config_agent_host(char *reply, size_t cap) {
        esp_err_t err = talk_is_listening(s_talk) || app_voice_live_active() || s_srv != SS_IDLE
            ? ESP_ERR_INVALID_STATE : muse_store_select(false);
        snprintf(reply, cap, "{\"ok\":%s,\"rebooting\":%s}", err == ESP_OK ? "true" : "false", err == ESP_OK ? "true" : "false");
        if (err == ESP_OK) app_post(EV_BOOT_LONG, 1, 0);
    }
static void dispatch_config(cJSON *j, const char *command, char *reply, size_t cap) {
    if (!strcmp(command, "set")) config_set(j, reply, cap);
    else if (!strcmp(command, "sim")) config_sim(j, reply, cap);
    else if (!strcmp(command, "speech")) app_speech_diagnostics(reply, cap);
    else if (!strcmp(command, "card")) config_card(j, reply, cap);
    else if (!strcmp(command, "models")) app_config_models(j, reply, cap);
    else if (!strcmp(command, "lcd")) config_lcd(reply, cap);
    else if (!strcmp(command, "reboot")) config_reboot(reply, cap);
    else if (!strcmp(command, "reset")) config_reset(reply, cap);
    else if (!strcmp(command, "muse_pair")) config_muse(j, reply, cap);
    else if (!strcmp(command, "agent_host")) config_agent_host(reply, cap);
    else config_info(reply, cap);
}
static bool config_contains_nul(const char *json, size_t len) {
    if (!json || memchr(json, 0, len)) return true;
    for (size_t i = 0; i < len; i++) {
        if (json[i] != '\\') continue;
        if (i + 5 < len && !memcmp(json + i + 1, "u0000", 5)) return true;
        if (i + 1 < len) i++; // An escaped backslash cannot introduce a Unicode escape.
    }
    return false;
}
static void *on_config_parse(const char *json, size_t len) {
    return config_contains_nul(json, len) ? NULL : cJSON_ParseWithLength(json, len);
}
static const char *config_command(cJSON *j) {
    return j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "cmd")) : NULL;
}
static void on_config_reply(void *parsed, char *reply, size_t cap) {
    cJSON *j = parsed;
    const char *command = config_command(j);
    if (!command) snprintf(reply, cap, "{\"ok\":false,\"error\":\"bad command\"}");
    else dispatch_config(j, command, reply, cap);
}
static void wipe_json_string(char *text) {
    if (!text) return;
    for (volatile char *p = text; *p; p++) *p = 0;
}
static void on_config_free(void *parsed) {
    cJSON *item = parsed;
    // Flatten the parsed tree using cJSON's first-child last-sibling pointer.
    // This uses constant stack even for a deeply nested, untrusted request.
    while (item) {
        cJSON *next = item->next;
        if (item->child && !(item->type & cJSON_IsReference)) {
            item->child->prev->next = next;
            next = item->child;
            item->child = NULL;
        }
        item->next = NULL;
        if (!(item->type & cJSON_IsReference)) wipe_json_string(item->valuestring);
        if (!(item->type & cJSON_StringIsConst)) wipe_json_string(item->string);
        cJSON_Delete(item);
        item = next;
    }
}
static bool on_config_info(void *parsed, status_json_sink_t sink, void *context, bool *ok) {
    const char *command = config_command(parsed);
    bool handled = command && !strcmp(command, "info");
    if (handled) {
        status_json_t info;
        status_json_begin_sink(&info, LINK_USB_CONFIG_REPLY_CAP, sink, context);
        app_config_info_fields(&info);
        *ok = status_json_end(&info);
    }
    return handled;
}
void app_protocol_handlers(link_handlers_t *handlers) {
    *handlers = (link_handlers_t){
        .on_json = on_json,
        .on_audio = on_audio,
        .on_link = on_link,
        .on_config_parse = on_config_parse, .on_config_info = on_config_info,
        .on_config_reply = on_config_reply, .on_config_free = on_config_free,
        .on_pair = on_pair,
    };
}
