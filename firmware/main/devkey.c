#include "devkey.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "app_nvs.h"
#include "settings.h"

static const char *TAG = "devkey";
static mbedtls_ecdsa_context s_key;
static SemaphoreHandle_t s_mtx;
static char s_pub[92];  // base64 of 65 bytes = 88 chars
static atomic_bool s_ready;

static int rng(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    esp_fill_random(out, len);  // hardware RNG (RF on: Wi-Fi is up before any signing)
    return 0;
}

static esp_err_t load(uint8_t d[32]) {
    esp_err_t err = app_nvs_lock();
    if (err != ESP_OK) return err;
    if (!app_nvs_ready_locked()) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    nvs_handle_t h;
    err = nvs_open("kubik", NVS_READONLY, &h);
    if (err != ESP_OK) goto done;
    size_t n = 32;
    err = nvs_get_blob(h, "devkey", d, &n);
    nvs_close(h);
    if (err == ESP_OK && n != 32) err = ESP_ERR_NVS_INVALID_LENGTH;
done:
    if (err != ESP_OK) mbedtls_platform_zeroize(d, 32);
    app_nvs_unlock();
    return err;
}

/** Recover and confirm whether a failed write left the expected value in NVS. */
static esp_err_t confirm_recovered_write(const uint8_t expected[32], esp_err_t write_error) {
    uint8_t check[32] = {0};
    esp_err_t err = app_nvs_recover_locked();
    if (err != ESP_OK) goto unhealthy;

    nvs_handle_t h;
    err = nvs_open("kubik", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) { err = write_error; goto done; }
    if (err != ESP_OK) goto unhealthy;
    size_t n = sizeof check;
    err = nvs_get_blob(h, "devkey", check, &n);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) { err = write_error; goto done; }
    if (err != ESP_OK) goto unhealthy;
    if (n != sizeof check) { err = ESP_ERR_NVS_INVALID_LENGTH; goto unhealthy; }
    if (memcmp(check, expected, sizeof check) != 0) { err = ESP_ERR_INVALID_STATE; goto unhealthy; }
    err = ESP_OK;
    goto done;

unhealthy:
    app_nvs_mark_unhealthy_locked();
done:
    mbedtls_platform_zeroize(check, sizeof check);
    return err;
}

static esp_err_t save(const uint8_t d[32]) {
    esp_err_t err = app_nvs_lock();
    if (err != ESP_OK) return err;
    if (!app_nvs_ready_locked()) { err = ESP_ERR_INVALID_STATE; goto done; }

    nvs_handle_t h;
    err = nvs_open("kubik", NVS_READWRITE, &h);
    if (err != ESP_OK) goto done;
    err = nvs_set_blob(h, "devkey", d, 32);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) err = confirm_recovered_write(d, err);
done:
    app_nvs_unlock();
    return err;
}

static int write_public_key(char public_key[92]) {
    uint8_t q[65];
    size_t qn = 0, bn = 0;
    int err = mbedtls_ecp_point_write_binary(&s_key.MBEDTLS_PRIVATE(grp), &s_key.MBEDTLS_PRIVATE(Q),
                                              MBEDTLS_ECP_PF_UNCOMPRESSED, &qn, q, sizeof q);
    if (!err) err = mbedtls_base64_encode((unsigned char *)public_key, 92, &bn, q, qn);
    if (!err && bn < 92) public_key[bn] = '\0';
    else if (!err) err = MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    mbedtls_platform_zeroize(q, sizeof q);
    return err;
}

static int prepare_key(uint8_t private_key[32], bool *created) {
    int err = load(private_key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (err == ESP_OK) {
        err = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &s_key, private_key, 32);
        if (!err) err = mbedtls_ecp_check_privkey(&s_key.MBEDTLS_PRIVATE(grp), &s_key.MBEDTLS_PRIVATE(d));
        if (!err) err = mbedtls_ecp_keypair_calc_public(&s_key, rng, NULL);
    } else {
        err = mbedtls_ecdsa_genkey(&s_key, MBEDTLS_ECP_DP_SECP256R1, rng, NULL);
        if (!err) err = mbedtls_ecp_write_key_ext(&s_key, &(size_t){0}, private_key, 32);
        if (!err) err = save(private_key);
        *created = err == ESP_OK;
    }
    return err;
}

bool devkey_init(void) {
    if (atomic_load_explicit(&s_ready, memory_order_acquire)) return true;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) {
        mbedtls_platform_zeroize(s_pub, sizeof s_pub);
        ESP_LOGE(TAG, "device signing mutex unavailable");
        return false;
    }

    uint8_t d[32] = {0};
    char public_key[92] = {0};
    bool created = false;
    mbedtls_ecdsa_init(&s_key);
    int err = prepare_key(d, &created);
    if (!err) err = write_public_key(public_key);
    if (err) goto failed;

    memcpy(s_pub, public_key, sizeof s_pub);
    atomic_store_explicit(&s_ready, true, memory_order_release);
    mbedtls_platform_zeroize(d, sizeof d);
    mbedtls_platform_zeroize(public_key, sizeof public_key);
    if (created) ESP_LOGI(TAG, "new device key created");
    return true;

failed:
    mbedtls_ecdsa_free(&s_key);
    mbedtls_platform_zeroize(d, sizeof d);
    mbedtls_platform_zeroize(public_key, sizeof public_key);
    mbedtls_platform_zeroize(s_pub, sizeof s_pub);
    ESP_LOGE(TAG, "device key unavailable (%d)", err);
    return false;
}

const char *devkey_public(void) {
    return atomic_load_explicit(&s_ready, memory_order_acquire) ? s_pub : "";
}

static bool valid_challenge(const char *nonce, const char *bind) {
    return nonce && nonce[0] && strlen(nonce) <= 64 && bind && bind[0];
}

static int format_challenge(const char *nonce, const char *bind, char message[320]) {
    int length = snprintf(message, 320, "kubik-auth-v5\n%s\n%s\n%s\n%s",
                          nonce, g_device_id, devkey_public(), bind);
    return length > 0 && length < 320 ? length : -1;
}

static int sign_message(const char *message, size_t length, uint8_t hash[32],
                        uint8_t signature[MBEDTLS_ECDSA_MAX_LEN], size_t *signature_size) {
    int err = mbedtls_sha256((const unsigned char *)message, length, hash, 0);
    if (err) return err;
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    err = mbedtls_ecdsa_write_signature(&s_key, MBEDTLS_MD_SHA256, hash, 32,
                                        signature, MBEDTLS_ECDSA_MAX_LEN, signature_size, rng, NULL);
    xSemaphoreGive(s_mtx);
    return err;
}

static int encode_signature(const uint8_t signature[MBEDTLS_ECDSA_MAX_LEN], size_t signature_size,
                            char *encoded, size_t capacity) {
    size_t encoded_size = 0;
    int err = mbedtls_base64_encode((unsigned char *)encoded, capacity, &encoded_size,
                                    signature, signature_size);
    if (!err && encoded_size < capacity) encoded[encoded_size] = '\0';
    else if (!err) err = MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    return err;
}

bool devkey_sign_challenge(const char *nonce, const char *bind, char *sig_b64, size_t cap) {
    if (sig_b64 && cap) sig_b64[0] = '\0';
    if (!atomic_load_explicit(&s_ready, memory_order_acquire) || !s_mtx || !sig_b64 || !cap ||
        !valid_challenge(nonce, bind)) return false;
    char msg[320];
    int length = format_challenge(nonce, bind, msg);
    if (length < 0) return false;
    uint8_t hash[32] = {0}, signature[MBEDTLS_ECDSA_MAX_LEN] = {0};
    size_t signature_size = 0;
    int err = sign_message(msg, (size_t)length, hash, signature, &signature_size);
    if (!err) err = encode_signature(signature, signature_size, sig_b64, cap);
    if (err) sig_b64[0] = '\0';
    mbedtls_platform_zeroize(hash, sizeof hash);
    mbedtls_platform_zeroize(signature, sizeof signature);
    return err == 0;
}
