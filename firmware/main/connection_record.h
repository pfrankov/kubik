#pragma once

#include <stdint.h>
#include "settings.h"
#include "muse_store.h"

#define CONNECTION_RECORD_MAGIC 0x4b434e32u
#define CONNECTION_RECORD_VERSION 3u
#define CONNECTION_RECORD_PIN_SET 0x0001u
#define CONNECTION_RECORD_KEY_A "connection_a"
#define CONNECTION_RECORD_KEY_B "connection_b"

typedef struct {
    uint32_t magic;
    uint8_t version, count, active, reserved;
    wifi_profile_t profiles[SETTINGS_WIFI_MAX];
    char url[128];
} connection_networks_t;

typedef struct connection_record {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint64_t revision;
    uint64_t network_revision;
    uint32_t muse_generation;
    connection_networks_t networks;
    uint8_t server_pin[SETTINGS_PIN_BYTES];
    muse_credentials_t muse;
} connection_record_t;

// Keep this the exact on-flash schema tested by the IDF host-NVS fixture.
#ifdef __cplusplus
static_assert(sizeof(wifi_profile_t) == 98, "Wi-Fi profile layout changed");
static_assert(sizeof(connection_networks_t) == 920, "network record layout changed");
static_assert(sizeof(muse_credentials_t) == 4204, "Muse credential layout changed");
static_assert(sizeof(connection_record_t) == 5184, "connection record layout changed");
#else
_Static_assert(sizeof(wifi_profile_t) == 98, "Wi-Fi profile layout changed");
_Static_assert(sizeof(connection_networks_t) == 920, "network record layout changed");
_Static_assert(sizeof(muse_credentials_t) == 4204, "Muse credential layout changed");
_Static_assert(sizeof(connection_record_t) == 5184, "connection record layout changed");
#endif
