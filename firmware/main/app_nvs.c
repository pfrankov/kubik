#include "app_nvs.h"

#include "nvs.h"
#include "nvs_flash.h"

#if !defined(ESP_PLATFORM) && defined(APP_NVS_TEST_LOCK)
#include <pthread.h>
static pthread_mutex_t s_test_mutex = PTHREAD_MUTEX_INITIALIZER;

bool app_nvs_test_lock_is_held(void) {
    if (pthread_mutex_trylock(&s_test_mutex) != 0) return true;
    pthread_mutex_unlock(&s_test_mutex);
    return false;
}
#endif

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"

static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t mutex_get_or_create(void) {
    portENTER_CRITICAL(&s_mutex_init_lock);
    SemaphoreHandle_t mutex = s_mutex;
    portEXIT_CRITICAL(&s_mutex_init_lock);
    if (mutex) return mutex;

    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (!candidate) return NULL;
    portENTER_CRITICAL(&s_mutex_init_lock);
    if (!s_mutex) {
        s_mutex = candidate;
        mutex = candidate;
        candidate = NULL;
    } else {
        mutex = s_mutex;
    }
    portEXIT_CRITICAL(&s_mutex_init_lock);
    if (candidate) vSemaphoreDelete(candidate);
    return mutex;
}
#endif

static bool s_ready;
static bool s_blocked;

esp_err_t app_nvs_lock(void) {
#ifdef ESP_PLATFORM
    SemaphoreHandle_t mutex = mutex_get_or_create();
    if (!mutex) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
#elif defined(APP_NVS_TEST_LOCK)
    if (pthread_mutex_lock(&s_test_mutex) != 0) return ESP_FAIL;
#endif
    return ESP_OK;
}

void app_nvs_unlock(void) {
#ifdef ESP_PLATFORM
    SemaphoreHandle_t mutex = s_mutex;
    if (mutex) xSemaphoreGive(mutex);
#elif defined(APP_NVS_TEST_LOCK)
    pthread_mutex_unlock(&s_test_mutex);
#endif
}

esp_err_t app_nvs_init_locked(void) {
    if (s_blocked) return ESP_ERR_INVALID_STATE;
    if (s_ready) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) s_ready = true;
    return err;
}

esp_err_t app_nvs_recover_locked(void) {
    s_ready = false;
    s_blocked = true;
    esp_err_t err = nvs_flash_deinit();
    if (err == ESP_OK) err = nvs_flash_init();
    if (err == ESP_OK) {
        s_ready = true;
        s_blocked = false;
    }
    return err;
}

bool app_nvs_ready_locked(void) { return s_ready && !s_blocked; }

void app_nvs_mark_unhealthy_locked(void) {
    s_ready = false;
    s_blocked = true;
}
