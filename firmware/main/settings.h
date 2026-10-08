// Persistent settings in NVS (namespace "kubik").
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "character.h"
#include "tess_progress.h"

struct connection_record;

#define SETTINGS_PIN_BYTES 32
#define SETTINGS_WIFI_MAX 8

typedef struct {
    char ssid[33];
    char password[65];
} wifi_profile_t;

typedef struct {
    // The saved list is authoritative. wifi_ssid/password name the profile the
    // station is currently trying (or the newest one from setup).
    wifi_profile_t wifi_profiles[SETTINGS_WIFI_MAX];
    uint8_t wifi_profile_count;
    char wifi_ssid[33];
    char wifi_pass[65];
    // "" finds OpenClaw on this network; kubik://host[:port] names it there;
    // wss://host/kubik/v1 is a TLS-protected public address.
    char server_url[128];
    // LAN mode: SHA-256 of the plugin's TLS key (SPKI), kept after its first welcome.
    uint8_t server_pin[SETTINGS_PIN_BYTES];
    bool server_pinned;
    char name[32];
    int volume;      // speech, 0..100; host volume contract
    int ui_volume;   // interface / character sounds, 0..100
    int brightness;  // 10..255
    bool greeted;    // first-run greeting played
    bool event_overlay; // show recent agent events above the home character
    bool guide_done; // first-use tour completed or explicitly skipped
    uint8_t tess_progress; // six one-time Tess game discoveries, TESS_PROGRESS_MASK
} settings_t;

extern settings_t g_settings;
extern char g_device_id[20];  // "kubik-xxxxxx", from the MAC

void settings_load(void);
void settings_save(void);
esp_err_t settings_complete_guide(void);
// Merge earned game milestones into their dedicated NVS key. The value is a
// cumulative snapshot; stale subsets never clear discoveries already saved.
esp_err_t settings_save_tess_progress(uint8_t progress);
esp_err_t settings_read_tess_progress(uint8_t *progress);
bool settings_take_provisioning(void);
// Saved networks and endpoint are one durable record. Exact SSID upserts
// replace its password; an empty password keeps an existing exact match.
// Saving is the user's explicit choice, so it also forgets the pinned server key.
esp_err_t settings_save_connection(const char *ssid, const char *password, const char *url);
// Setup commits the candidate connection and Muse agent selection as one NVS record.
esp_err_t settings_save_setup(const char *ssid, const char *password, const char *url,
                              bool muse_selected, const char *sdk_token);
void settings_apply_connection_record(const struct connection_record *record);
void settings_apply_connection_pin(const struct connection_record *record);
int settings_wifi_find(const char *ssid);
esp_err_t settings_save_pin(const uint8_t pin[SETTINGS_PIN_BYTES]);
// Erases the app namespace. Runtime settings change only when the erase succeeds.
esp_err_t settings_factory_reset(void);
bool settings_server_valid(const char *url);
