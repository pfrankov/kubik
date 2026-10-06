#include "link_pin.h"

#include <ctype.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "settings.h"

static const char *TAG = "pin";
static const char HEX[] = "0123456789abcdef";

// The Wi-Fi handshake writes its key from the WebSocket task before any server frame
// can arrive; everything else runs under link's route lock.
static uint8_t s_wifi_key[SETTINGS_PIN_BYTES];
static atomic_bool s_wifi_seen, s_mismatch;
static uint8_t s_usb_key[SETTINGS_PIN_BYTES];
static char s_usb_bind[LINK_BIND_MAX];
static uint32_t s_usb_epoch;
static bool s_usb_seen;

server_mode_t link_server_mode(const char *url) {
    if (!url[0]) return SERVER_LAN_DISCOVER;
    if (!strncmp(url, "kubik://", 8)) return SERVER_LAN_FIXED;
    return strncmp(url, "wss://", 6) ? SERVER_INVALID : SERVER_CA;
}

static bool lan_mode(void) { return link_server_mode(g_settings.server_url) <= SERVER_LAN_FIXED; }

static bool matches_pin(const uint8_t key[SETTINGS_PIN_BYTES]) {
    if (!g_settings.server_pinned || !memcmp(key, g_settings.server_pin, SETTINGS_PIN_BYTES)) return true;
    ESP_LOGW(TAG, "server key changed; save the connection again to trust the new one");
    atomic_store(&s_mismatch, true);
    return false;
}

// mbedTLS calls this for each certificate of the chain, the leaf last (depth 0).
static int verify_pinned(void *context, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)context;
    // The self-signed leaf is never in a CA chain; trust rests on its key alone.
    if (depth > 0) {
        *flags = 0;
        return 0;
    }
    uint8_t key[SETTINGS_PIN_BYTES];
    if (mbedtls_sha256(crt->pk_raw.p, crt->pk_raw.len, key, 0) != 0) return MBEDTLS_ERR_X509_FATAL_ERROR;
    if (!matches_pin(key)) {
        *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        return 0;
    }
    memcpy(s_wifi_key, key, sizeof key);
    atomic_store(&s_wifi_seen, true);
    *flags = 0;
    return 0;
}

esp_err_t link_pin_attach(void *ssl_conf) {
    // A non-empty CA chain makes mbedTLS verify the peer; this one trusts nothing.
    static mbedtls_x509_crt no_authority;
    mbedtls_ssl_conf_ca_chain(ssl_conf, &no_authority, NULL);
    mbedtls_ssl_conf_verify(ssl_conf, verify_pinned, NULL);
    return ESP_OK;
}

void link_pin_begin_wifi(void) { atomic_store(&s_wifi_seen, false); }

static bool parse_key(const char *hex, uint8_t key[SETTINGS_PIN_BYTES]) {
    if (strlen(hex) != SETTINGS_PIN_BYTES * 2) return false;
    for (int i = 0; i < SETTINGS_PIN_BYTES * 2; i++) {
        const char *digit = strchr(HEX, hex[i]);
        if (!digit) return false;
        key[i / 2] = (uint8_t)(key[i / 2] << 4 | (digit - HEX));
    }
    return true;
}

void link_pin_usb_bind(uint32_t epoch, const char *bind, size_t len) {
    s_usb_seen = false;
    if (!len || len >= LINK_BIND_MAX) return;
    memcpy(s_usb_bind, bind, len);
    s_usb_bind[len] = 0;
    // Only "ca:<host>", "none" or a hex key: nothing that could reshape the signed lines.
    if (strspn(s_usb_bind, "0123456789abcdefghijklmnopqrstuvwxyz.:-") != len) return;
    if (lan_mode() && (!parse_key(s_usb_bind, s_usb_key) || !matches_pin(s_usb_key))) return;
    s_usb_epoch = epoch;
    s_usb_seen = true;
}

// "ca:<host>" for the wss://host[:port]/path server setting (settings.c has checked its host).
static bool ca_bind(char out[LINK_BIND_MAX]) {
    const char *host = g_settings.server_url + strlen("wss://"), *end = host;
    if (*host == '[') end = strchr(++host, ']');
    else while (*end && *end != ':' && *end != '/') end++;
    if (!end || end == host) return false;  // the URL is under 128 bytes: the bind always fits
    memcpy(out, "ca:", 3);
    for (const char *p = host; p < end; p++) out[3 + (p - host)] = (char)tolower((unsigned char)*p);
    out[3 + (end - host)] = 0;
    return true;
}

static bool wifi_bind(char out[LINK_BIND_MAX]) {
    server_mode_t mode = link_server_mode(g_settings.server_url);
    if (mode == SERVER_CA) return ca_bind(out);
    if (mode == SERVER_INVALID) return false;
    if (!atomic_load(&s_wifi_seen)) return false;
    for (int i = 0; i < SETTINGS_PIN_BYTES; i++) {
        out[2 * i] = HEX[s_wifi_key[i] >> 4];
        out[2 * i + 1] = HEX[s_wifi_key[i] & 15];
    }
    out[SETTINGS_PIN_BYTES * 2] = 0;
    return true;
}

bool link_pin_bind(bool usb, uint32_t epoch, char out[LINK_BIND_MAX]) {
    if (!usb) return wifi_bind(out);
    if (!s_usb_seen || s_usb_epoch != epoch) return false;
    strcpy(out, s_usb_bind);
    return true;
}

void link_pin_keep(bool usb, uint32_t epoch) {
    if (g_settings.server_pinned || !lan_mode()) return;
    const uint8_t *key = usb ? s_usb_key : s_wifi_key;
    bool seen = usb ? s_usb_seen && s_usb_epoch == epoch : atomic_load(&s_wifi_seen);
    if (!seen) return;
    esp_err_t err = settings_save_pin(key);
    if (err == ESP_OK) ESP_LOGI(TAG, "server key pinned");
    else ESP_LOGW(TAG, "could not keep the server key: %s", esp_err_to_name(err));
}

bool link_pin_take_mismatch(void) { return atomic_exchange(&s_mismatch, false); }
