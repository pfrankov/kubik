#include "app_config_info.h"
#include "app_internal.h"
#include "app_status.h"
#include "app_emoji.h"
#include "app_lab.h"
#include "app_journal.h"
#include "audio.h"
#include "board.h"
#include "devkey.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_clk.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "heap_probe.h"
#include "muse_store.h"
#include "printer_protocol.h"
#include "settings.h"
#include "setup.h"
#include "version.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>

static void agent_status_snapshot(char *status, size_t status_cap, char *model, size_t model_cap,
                                  bool *open, unsigned *cursor, unsigned *total) {
    face_lock();
    const agent_menu_t *agent = &g_face.agent;
    *open = agent->open;
    *cursor = agent->cursor;
    *total = agent->total;
    snprintf(model, model_cap, "%.160s", agent->model);
    const char *name = agent->error[0] ? agent->error :
        agent->request_pending ? agent->waiting_kind == AGENT_REQUEST_SELECT ? "saving" : "loading" :
        agent->wire_rid ? "loading" : !agent->online ? "offline" : agent->capabilities_known ? "ready" : "unknown";
    snprintf(status, status_cap, "%s", name);
    face_unlock();
}
static void agent_capabilities_snapshot(bool *known, bool *stt, bool *tts) {
    face_lock();
    *known = g_face.agent.capabilities_known;
    *stt = g_face.agent.stt_available;
    *tts = g_face.agent.tts_available;
    face_unlock();
}
static void add_agent_view(status_json_t *info) {
    static const char *const views[] = {"overview", "models", "guide", "voice_modes", "classic"};
    static const char *const modes[] = {"classic", "realtime", "live"};
    status_json_string(info, "voice_mode", modes[app_voice_mode()]);
    status_json_bool(info, "live_active", app_voice_live_active());
    status_json_bool(info, "live_ready", app_voice_live_ready());
    face_lock();
    unsigned view = (unsigned)g_face.agent.view;
    unsigned loaded = g_face.agent.count, listed = agent_menu_listed(&g_face.agent);
    int scroll = (int)g_face.agent.scroll;
    unsigned guide_step = g_face.agent.guide_step;
    bool card_open = g_face.card_n > 0, printer_open = g_face.printer.open;
    face_unlock();
    status_json_string(info, "agent_view", view < sizeof views / sizeof views[0] ? views[view] : "overview");
    status_json_integer(info, "guide_step", guide_step);
    status_json_bool(info, "card_open", card_open);
    status_json_bool(info, "printer_open", printer_open);
    status_json_bool(info, "pair_visible", s_pair_code[0] && !s_pair_hidden && !s_setup);
    status_json_bool(info, "guide_done", g_settings.guide_done);
    app_journal_info(info);
    app_status_info(info); app_emoji_info(info);
    status_json_integer(info, "agent_loaded", loaded);
    status_json_integer(info, "agent_listed", listed);
    status_json_integer(info, "agent_scroll", scroll);
}
void app_config_info_fields(status_json_t *info) {
    power_status_t power;
    pmic_read(&power);
    audio_mic_status_t mic;
    audio_mic_status(&mic);
    char agent_status[AGENT_ERROR_MAX + 1], agent_model[AGENT_MODEL_ID_MAX + 1];
    bool agent_open;
    bool stt_known, stt_available, tts_available;
    unsigned agent_cursor, agent_total;
    agent_status_snapshot(agent_status, sizeof agent_status, agent_model, sizeof agent_model,
                          &agent_open, &agent_cursor, &agent_total);
    agent_capabilities_snapshot(&stt_known, &stt_available, &tts_available);
    status_json_bool(info, "ok", true);
    status_json_string(info, "device", g_device_id);
    status_json_string(info, "fw", KUBIK_FW_VERSION);
    status_json_integer(info, "uptime_ms", esp_timer_get_time() / 1000);
    status_json_string(info, "ssid", g_settings.wifi_ssid);
    status_json_string(info, "url", g_settings.server_url);
    status_json_string(info, "agent", settings_agent_name());
    status_json_string(info, "key", devkey_public());
    if (setup_active()) {
        setup_labels_t labels;
        setup_labels(&labels);
        status_json_string(info, "setup_ap", labels.ap_ssid);
        status_json_string(info, "setup_pass", labels.ap_pass);
    }
    status_json_integer(info, "volume", g_settings.volume);
    status_json_integer(info, "ui_volume", g_settings.ui_volume);
    status_json_bool(info, "sound_menu", g_face.menu.sound);
    status_json_integer(info, "brightness", g_settings.brightness);
    status_json_string(info, "character", character_name(KUBIK_CHARACTER));
    status_json_string(info, "via", link_via());
    status_json_bool(info, "online", s_online);
    status_json_bool(info, "offline_play", s_offline_play);
    status_json_bool(info, "menu", s_menu);
    status_json_bool(info, "screen_lab", app_lab_active());
    status_json_string(info, "lab_screen", app_lab_screen());
    status_json_bool(info, "agent_open", agent_open);
    status_json_integer(info, "agent_page", agent_cursor);
    status_json_integer(info, "agent_total", agent_total);
    status_json_string(info, "agent_status", agent_status);
    status_json_string(info, "agent_model", agent_model);
    add_agent_view(info);
    status_json_bool(info, "stt_known", stt_known);
    status_json_bool(info, "stt_available", stt_available);
    status_json_bool(info, "tts_available", tts_available);
    status_json_bool(info, "audio_dozing", mic.dozing);
    status_json_bool(info, "wake_listening", app_wake_listening());
    status_json_bool(info, "auto_recording", app_wake_recording());
    status_json_integer(info, "wake_inference_max_us", app_wake_max_us());
    status_json_integer(info, "wake_probability", app_wake_probability());
    status_json_integer(info, "wake_peak_probability", app_wake_peak_probability());
    status_json_integer(info, "wake_inference_average_us", app_wake_average_us());
    status_json_bool(info, "mic_open", mic.open);
    status_json_bool(info, "mic_enabled", mic.enabled);
    status_json_bool(info, "mic_rx_enabled", mic.rx_enabled);
    status_json_integer(info, "mic_reads", mic.reads);
    status_json_integer(info, "mic_idle_reads", mic.idle_reads);
    status_json_bool(info, "screen_dark", power_is_dark(s_power));
    status_json_bool(info, "screen_dimmed", s_power == PWR_DIMMED);
    status_json_integer(info, "cpu_mhz", esp_clk_cpu_freq() / 1000000);
    esp_pm_config_t clocks; ESP_ERROR_CHECK(esp_pm_get_configuration(&clocks));
    status_json_integer(info, "cpu_min_mhz", clocks.min_freq_mhz);
    status_json_integer(info, "cpu_max_mhz", clocks.max_freq_mhz);
    status_json_string(info, "chip", CONFIG_IDF_TARGET);
    status_json_bool(info, "screen_sleeping", power_is_asleep(s_power));
    status_json_integer(info, "idle_ms", now_ms() - s_last_activity);
    status_json_bool(info, "sfx_playing", audio_sfx_playing());
    status_json_integer(info, "battery_mv", power.battery_mv);
    status_json_integer(info, "battery_pct", power.battery_pct);
    status_json_bool(info, "charging", power.charging);
    status_json_bool(info, "usb_power", power.usb_power);
    status_json_bool(info, "power_known", power.power_known);
    status_json_bool(info, "wifi_connected", wifi_sta_connected());
    status_json_bool(info, "server_pinned", g_settings.server_pinned);
    status_json_bool(info, "wifi_radio_started", wifi_radio_started());
    status_json_integer(info, "network_phase", app_network_phase());
    status_json_integer(info, "network_transitions", app_network_transitions());
    status_json_bool(info, "network_diagnostic", now_ms() < s_debug_network_until);
    status_json_bool(info, "cpu_light_sleep", power_light_sleep_enabled());
    status_json_integer(info, "heap", esp_get_free_heap_size());
    status_json_integer(info, "heap_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    status_json_integer(info, "heap_dma", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    status_json_integer(info, "heap_min", esp_get_minimum_free_heap_size());
    status_json_integer(info, "app_stack_free", app_stack_free());
    status_json_integer(info, "heap_failures", hp_allocation_failures());
    printer_protocol_info(info);
    muse_state_t muse = muse_store_state(); status_json_string(info, "native_agent", muse == MUSE_OFF ? "" : "Muse");
    status_json_bool(info, "muse_pairing", muse == MUSE_PAIRING);
}
