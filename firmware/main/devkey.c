#include "devkey.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "devkey";
static mbedtls_ecdsa_context s_key;
static SemaphoreHandle_t s_mtx;
static char s_pub[92];  // base64 of 65 bytes = 88 chars

static int rng(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    esp_fill_random(out, len);  // hardware RNG (RF on: Wi-Fi is up before any signing)
    return 0;
}

static bool load(uint8_t d[32]) {
    nvs_handle_t h;
    if (nvs_open("kubik", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = 32;
    bool ok = nvs_get_blob(h, "devkey", d, &n) == ESP_OK && n == 32;
    nvs_close(h);
    return ok;
}

static bool save(const uint8_t d[32]) {
    nvs_handle_t h;
    if (nvs_open("kubik", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "devkey", d, 32) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool devkey_init(void) {
    if (s_pub[0]) return true;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    uint8_t d[32];
    mbedtls_ecdsa_init(&s_key);
    int err;
    if (load(d)) {
        err = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &s_key, d, sizeof d);
        if (!err) err = mbedtls_ecp_keypair_calc_public(&s_key, rng, NULL);
    } else {
        err = mbedtls_ecdsa_genkey(&s_key, MBEDTLS_ECP_DP_SECP256R1, rng, NULL);
        if (!err) err = mbedtls_ecp_write_key_ext(&s_key, &(size_t){0}, d, sizeof d);
        if (!err && !save(d)) err = -1;
        if (!err) ESP_LOGI(TAG, "new device key created");
    }
    uint8_t q[65];
    size_t qn = 0, bn = 0;
    if (!err) err = mbedtls_ecp_point_write_binary(&s_key.MBEDTLS_PRIVATE(grp), &s_key.MBEDTLS_PRIVATE(Q),
                                                   MBEDTLS_ECP_PF_UNCOMPRESSED, &qn, q, sizeof q);
    if (!err) err = mbedtls_base64_encode((unsigned char *)s_pub, sizeof s_pub, &bn, q, qn);
    memset(d, 0, sizeof d);
    if (err) {
        ESP_LOGE(TAG, "device key unavailable (%d)", err);
        s_pub[0] = 0;
        return false;
    }
    return true;
}

const char *devkey_public(void) { return s_pub; }

bool devkey_sign_challenge(const char *nonce, const char *bind, char *sig_b64, size_t cap) {
    if (!s_pub[0] || !nonce || !nonce[0] || strlen(nonce) > 64 || !bind || !bind[0]) return false;
    char msg[320];
    int n = snprintf(msg, sizeof msg, "kubik-auth-v5\n%s\n%s\n%s\n%s", nonce, g_device_id, s_pub, bind);
    if (n <= 0 || n >= (int)sizeof msg) return false;
    uint8_t hash[32], sig[MBEDTLS_ECDSA_MAX_LEN];
    size_t sn = 0, bn = 0;
    if (mbedtls_sha256((const unsigned char *)msg, n, hash, 0)) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int err = mbedtls_ecdsa_write_signature(&s_key, MBEDTLS_MD_SHA256, hash, sizeof hash, sig, sizeof sig, &sn, rng, NULL);
    xSemaphoreGive(s_mtx);
    if (!err) err = mbedtls_base64_encode((unsigned char *)sig_b64, cap, &bn, sig, sn);
    return !err;
}
