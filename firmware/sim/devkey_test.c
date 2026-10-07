#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_nvs.h"
#include "devkey.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#define BLOB_CAPACITY 96

typedef enum {
    FAULT_NONE,
    FAULT_OPEN_INITIAL,
    FAULT_READ_INITIAL,
    FAULT_SET_BEFORE,
    FAULT_SET_AFTER,
    FAULT_COMMIT_BEFORE,
    FAULT_COMMIT_AFTER,
    FAULT_RECOVERY_OPEN,
    FAULT_RECOVERY_READ,
    FAULT_RECOVERY_INIT,
} fault_t;

typedef enum {
    CASE_MISSING,
    CASE_RESTART,
    CASE_MUTEX_FAILURE,
    CASE_LOAD_FAILURE,
    CASE_INVALID_SCALAR,
    CASE_SET_BEFORE,
    CASE_RECOVERED_WRITE,
    CASE_RECOVERY_READ_FAILURE,
    CASE_RECOVERY_INIT_FAILURE,
} case_kind_t;

typedef struct {
    const char *name;
    case_kind_t kind;
    fault_t fault;
} case_definition_t;

static const case_definition_t s_cases[] = {
    {"missing", CASE_MISSING, FAULT_NONE},
    {"restart", CASE_RESTART, FAULT_NONE},
    {"mutex-failure", CASE_MUTEX_FAILURE, FAULT_NONE},
    {"open-eio", CASE_LOAD_FAILURE, FAULT_OPEN_INITIAL},
    {"read-eio", CASE_LOAD_FAILURE, FAULT_READ_INITIAL},
    {"short", CASE_LOAD_FAILURE, FAULT_NONE},
    {"long", CASE_LOAD_FAILURE, FAULT_NONE},
    {"bad-zero", CASE_INVALID_SCALAR, FAULT_NONE},
    {"bad-order", CASE_INVALID_SCALAR, FAULT_NONE},
    {"set-before", CASE_SET_BEFORE, FAULT_SET_BEFORE},
    {"set-after", CASE_RECOVERED_WRITE, FAULT_SET_AFTER},
    {"commit-before", CASE_RECOVERED_WRITE, FAULT_COMMIT_BEFORE},
    {"commit-after", CASE_RECOVERED_WRITE, FAULT_COMMIT_AFTER},
    {"recovery-open-eio", CASE_RECOVERY_READ_FAILURE, FAULT_RECOVERY_OPEN},
    {"recovery-read-eio", CASE_RECOVERY_READ_FAILURE, FAULT_RECOVERY_READ},
    {"recovery-init-fail", CASE_RECOVERY_INIT_FAILURE, FAULT_RECOVERY_INIT},
};

static char s_store_path[1024];
static uint8_t s_blob[BLOB_CAPACITY];
static size_t s_blob_size;
static bool s_present;
static fault_t s_fault;
static unsigned s_flash_init_calls, s_read_opens, s_reads, s_writes;
static unsigned s_open_writes, s_commits;
static pthread_mutex_t s_device_mutex = PTHREAD_MUTEX_INITIALIZER;
int devkey_test_fail_mutex;
char g_device_id[20] = "kubik-123456";

static bool read_store_file(void) {
    FILE *file = fopen(s_store_path, "rb");
    if (!file) {
        if (errno == ENOENT) { s_present = false; s_blob_size = 0; return true; }
        return false;
    }
    uint32_t size;
    bool ok = fread(&size, sizeof size, 1, file) == 1 && size <= sizeof s_blob;
    if (ok) {
        s_blob_size = size;
        ok = size == 0 || fread(s_blob, 1, size, file) == size;
        if (ok) ok = fgetc(file) == EOF;
    }
    if (fclose(file) != 0) ok = false;
    s_present = ok;
    return ok;
}

static bool write_store_file(const uint8_t *blob, size_t size) {
    char temporary[sizeof s_store_path + 8];
    int length = snprintf(temporary, sizeof temporary, "%s.tmp", s_store_path);
    if (length <= 0 || (size_t)length >= sizeof temporary) return false;
    FILE *file = fopen(temporary, "wb");
    if (!file) return false;
    uint32_t stored_size = (uint32_t)size;
    bool ok = fwrite(&stored_size, sizeof stored_size, 1, file) == 1
        && (size == 0 || fwrite(blob, 1, size, file) == size)
        && fflush(file) == 0;
    if (fclose(file) != 0) ok = false;
    if (ok) ok = rename(temporary, s_store_path) == 0;
    if (!ok) unlink(temporary);
    return ok;
}

static bool persist_blob(const uint8_t *blob, size_t size) {
    if (!write_store_file(blob, size)) return false;
    memcpy(s_blob, blob, size);
    s_blob_size = size;
    s_present = true;
    return true;
}

static bool same_store_file(const uint8_t *bytes, size_t size) {
    FILE *file = fopen(s_store_path, "rb");
    if (!file) return size == 0 && errno == ENOENT;
    uint8_t actual[sizeof s_blob + sizeof(uint32_t)];
    size_t count = fread(actual, 1, sizeof actual, file);
    bool ok = fclose(file) == 0 && count == size && memcmp(actual, bytes, size) == 0;
    return ok;
}

static void expect_no_key(const uint8_t *original_file, size_t original_size, unsigned write_attempts) {
    char signature[160] = "not-empty";
    assert(devkey_public()[0] == '\0');
    assert(!devkey_sign_challenge("nonce", "ca:test", signature, sizeof signature));
    assert(signature[0] == '\0');
    assert(s_writes == write_attempts);
    assert(same_store_file(original_file, original_size));
}

static void assert_nvs_healthy(void) {
    assert(app_nvs_lock() == ESP_OK);
    bool ready = app_nvs_ready_locked();
    app_nvs_unlock();
    assert(ready);
}

static void assert_nvs_blocked(void) {
    assert(app_nvs_lock() == ESP_OK);
    esp_err_t err = app_nvs_init_locked();
    app_nvs_unlock();
    assert(err == ESP_ERR_INVALID_STATE);
    assert(!app_nvs_ready_locked());
}

static void verify_signature(void) {
    const char *bind = "ca:test";
    const char *nonce = "devkey-test-nonce";
    char signature_b64[160] = {0};
    assert(devkey_sign_challenge(nonce, bind, signature_b64, sizeof signature_b64));

    uint8_t public_point[65], signature[MBEDTLS_ECDSA_MAX_LEN], hash[32];
    size_t point_size = 0, signature_size = 0;
    assert(mbedtls_base64_decode(public_point, sizeof public_point, &point_size,
        (const unsigned char *)devkey_public(), strlen(devkey_public())) == 0);
    assert(point_size == sizeof public_point && public_point[0] == 0x04);
    assert(mbedtls_base64_decode(signature, sizeof signature, &signature_size,
        (const unsigned char *)signature_b64, strlen(signature_b64)) == 0);

    char message[320];
    int length = snprintf(message, sizeof message, "kubik-auth-v5\n%s\n%s\n%s\n%s",
        nonce, g_device_id, devkey_public(), bind);
    assert(length > 0 && (size_t)length < sizeof message);
    assert(mbedtls_sha256((const unsigned char *)message, (size_t)length, hash, 0) == 0);
    mbedtls_ecdsa_context verifier;
    mbedtls_ecdsa_init(&verifier);
    assert(mbedtls_ecp_group_load(&verifier.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1) == 0);
    assert(mbedtls_ecp_point_read_binary(&verifier.MBEDTLS_PRIVATE(grp), &verifier.MBEDTLS_PRIVATE(Q),
        public_point, point_size) == 0);
    assert(mbedtls_ecdsa_read_signature(&verifier, hash, sizeof hash, signature, signature_size) == 0);
    mbedtls_ecdsa_free(&verifier);
}

static void expect_signer_ready(void) {
    assert(devkey_init());
    assert(strlen(devkey_public()) == 88);
    verify_signature();
    assert(s_present && s_blob_size == 32);
}

void esp_fill_random(void *output, size_t size) {
    mbedtls_entropy_context entropy;
    mbedtls_entropy_init(&entropy);
    assert(mbedtls_entropy_func(&entropy, output, size) == 0);
    mbedtls_entropy_free(&entropy);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return devkey_test_fail_mutex ? NULL : &s_device_mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t wait) {
    (void)wait;
    return mutex && pthread_mutex_lock(mutex) == 0 ? pdTRUE : 0;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
    return mutex && pthread_mutex_unlock(mutex) == 0 ? pdTRUE : 0;
}

esp_err_t nvs_flash_init(void) {
    s_flash_init_calls++;
    if (s_fault == FAULT_RECOVERY_INIT && s_flash_init_calls > 1) return ESP_FAIL;
    return read_store_file() ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_flash_deinit(void) {
    return ESP_OK;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    assert(strcmp(name, "kubik") == 0);
    if (mode == NVS_READONLY) {
        s_read_opens++;
        if ((s_fault == FAULT_OPEN_INITIAL && s_flash_init_calls == 1)
            || (s_fault == FAULT_RECOVERY_OPEN && s_flash_init_calls > 1)) return ESP_FAIL;
        if (!s_present) return ESP_ERR_NVS_NOT_FOUND;
        *handle = 1;
        return ESP_OK;
    }
    assert(mode == NVS_READWRITE);
    s_open_writes++;
    *handle = 2;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size) {
    assert(handle == 1 && strcmp(key, "devkey") == 0 && size);
    s_reads++;
    if ((s_fault == FAULT_READ_INITIAL && s_flash_init_calls == 1)
        || (s_fault == FAULT_RECOVERY_READ && s_flash_init_calls > 1)) return ESP_FAIL;
    if (!s_present) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < s_blob_size) { *size = s_blob_size; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(data, s_blob, s_blob_size);
    *size = s_blob_size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size) {
    assert(handle == 2 && strcmp(key, "devkey") == 0 && size <= sizeof s_blob);
    s_writes++;
    if (s_writes == 1 && s_fault == FAULT_SET_BEFORE) return ESP_FAIL;
    if (s_writes == 1 && s_fault == FAULT_RECOVERY_INIT) return ESP_FAIL;
    assert(persist_blob(data, size));
    if (s_writes == 1 && (s_fault == FAULT_SET_AFTER || s_fault == FAULT_RECOVERY_OPEN
        || s_fault == FAULT_RECOVERY_READ)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 2);
    s_commits++;
    if (s_commits == 1 && (s_fault == FAULT_COMMIT_BEFORE || s_fault == FAULT_COMMIT_AFTER)) return ESP_FAIL;
    return ESP_OK;
}

static void remove_store(void) {
    unlink(s_store_path);
    s_present = false;
    s_blob_size = 0;
}

static void capture_store(uint8_t original_file[sizeof s_blob + sizeof(uint32_t)], size_t *original_size) {
    FILE *file = fopen(s_store_path, "rb");
    *original_size = 0;
    if (file) {
        *original_size = fread(original_file, 1, sizeof s_blob + sizeof(uint32_t), file);
        assert(!ferror(file));
        assert(fclose(file) == 0);
    } else assert(errno == ENOENT);
}

static const case_definition_t *find_case(const char *name) {
    for (size_t i = 0; i < sizeof s_cases / sizeof s_cases[0]; ++i)
        if (!strcmp(name, s_cases[i].name)) return &s_cases[i];
    return NULL;
}

static void initialize_nvs(void) {
    assert(app_nvs_lock() == ESP_OK);
    assert(app_nvs_init_locked() == ESP_OK);
    app_nvs_unlock();
}

static void test_missing(void) {
    assert(!s_present);
    expect_signer_ready();
}

static void test_restart(const char *expected_public) {
    assert(expected_public && s_present && s_blob_size == 32);
    assert(devkey_init());
    assert(strcmp(devkey_public(), expected_public) == 0);
    verify_signature();
    assert(s_writes == 0);
}

static void test_mutex_failure(const uint8_t *original_file, size_t original_size) {
    devkey_test_fail_mutex = 1;
    expect_no_key(original_file, original_size, 0);
    assert(s_read_opens == 0 && s_open_writes == 0);
    devkey_test_fail_mutex = 0;
    expect_signer_ready();
}

static void test_load_failure(const uint8_t *original_file, size_t original_size) {
    assert(!devkey_init());
    expect_no_key(original_file, original_size, 0);
}

static void test_invalid_scalar(const uint8_t *original_file, size_t original_size) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        assert(!devkey_init());
        expect_no_key(original_file, original_size, 0);
    }
    remove_store();
    expect_signer_ready();
}

static void test_set_before(const uint8_t *original_file, size_t original_size) {
    assert(!devkey_init());
    expect_no_key(original_file, original_size, 1);
    assert_nvs_healthy();
    s_fault = FAULT_NONE;
    expect_signer_ready();
}

static void test_recovered_write(void) {
    expect_signer_ready();
    assert_nvs_healthy();
    assert(s_writes == 1);
}

static void test_recovery_read_failure(void) {
    assert(!devkey_init());
    assert_nvs_blocked();
    assert(devkey_public()[0] == '\0');
    char signature[160] = {0};
    assert(!devkey_sign_challenge("nonce", "ca:test", signature, sizeof signature));
    assert(s_present && s_blob_size == 32 && s_writes == 1);
}

static void test_recovery_init_failure(void) {
    assert(!devkey_init());
    assert_nvs_blocked();
    assert(devkey_public()[0] == '\0');
    assert(!s_present && s_writes == 1);
}

static void run_case(const char *scenario, const char *expected_public) {
    uint8_t original_file[sizeof s_blob + sizeof(uint32_t)] = {0};
    size_t original_size;
    capture_store(original_file, &original_size);
    const case_definition_t *test = find_case(scenario);
    assert(test);
    if (!test) abort();
    s_fault = test->fault;
    initialize_nvs();

    switch (test->kind) {
    case CASE_MISSING: test_missing(); break;
    case CASE_RESTART: test_restart(expected_public); break;
    case CASE_MUTEX_FAILURE: test_mutex_failure(original_file, original_size); break;
    case CASE_LOAD_FAILURE: test_load_failure(original_file, original_size); break;
    case CASE_INVALID_SCALAR: test_invalid_scalar(original_file, original_size); break;
    case CASE_SET_BEFORE: test_set_before(original_file, original_size); break;
    case CASE_RECOVERED_WRITE: test_recovered_write(); break;
    case CASE_RECOVERY_READ_FAILURE: test_recovery_read_failure(); break;
    case CASE_RECOVERY_INIT_FAILURE: test_recovery_init_failure(); break;
    }

    printf("PASS %s", scenario);
    if (!strcmp(scenario, "missing")) printf(" PUBLIC=%s", devkey_public());
    putchar('\n');
}

int main(int argc, char **argv) {
    assert(argc >= 3);
    assert(strlen(argv[2]) < sizeof s_store_path);
    strcpy(s_store_path, argv[2]);
    run_case(argv[1], argc > 3 ? argv[3] : NULL);
    return 0;
}
