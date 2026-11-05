#pragma once
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERROR_CHECK(value) assert((value)==ESP_OK)
