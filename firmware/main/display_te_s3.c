#include "display_te_s3.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TE_WAIT_MS 40
static SemaphoreHandle_t s_te_edge;
static portMUX_TYPE s_edge_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_edge_us, s_accepted_edge_us;
static uint32_t s_period_us;

static void te_edge(void *unused) {
    (void)unused;
    int64_t now = esp_timer_get_time();  // ESP-IDF documents this as ISR-safe and lock-free.
    portENTER_CRITICAL_ISR(&s_edge_lock);
    int64_t period = now - s_edge_us;
    s_period_us = s_edge_us && period >= 10000 && period <= 25000 ? (uint32_t)period : 0;
    s_edge_us = now;
    portEXIT_CRITICAL_ISR(&s_edge_lock);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_te_edge, &woken);
    if (woken) portYIELD_FROM_ISR();
}

esp_err_t display_te_init(void) {
    if (s_te_edge) return ESP_ERR_INVALID_STATE;
    s_te_edge = xSemaphoreCreateBinary();
    if (!s_te_edge) return ESP_ERR_NO_MEM;
    const gpio_config_t config = {.pin_bit_mask = 1ULL << PIN_LCD_TE,
        .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_POSEDGE};
    esp_err_t result = gpio_config(&config);
    if (result == ESP_OK) {
        result = gpio_install_isr_service(0);
        if (result == ESP_ERR_INVALID_STATE) result = ESP_OK;  // Share the existing service.
    }
    if (result == ESP_OK) result = gpio_isr_handler_add(PIN_LCD_TE, te_edge, NULL);
    if (result != ESP_OK) {
        vSemaphoreDelete(s_te_edge);
        s_te_edge = NULL;
    }
    return result;
}

esp_err_t display_te_wait(void) {
    if (!s_te_edge) return ESP_ERR_INVALID_STATE;
    board_display_wait_idle();
    // Binary semaphore: one nonblocking take discards every preceding edge.
    xSemaphoreTake(s_te_edge, 0);
    if (xSemaphoreTake(s_te_edge, pdMS_TO_TICKS(TE_WAIT_MS)) != pdTRUE) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&s_edge_lock);
    s_accepted_edge_us = s_edge_us;
    portEXIT_CRITICAL(&s_edge_lock);
    return ESP_OK;
}

int64_t display_te_last_edge_us(void) {
    portENTER_CRITICAL(&s_edge_lock);
    int64_t edge = s_accepted_edge_us;
    portEXIT_CRITICAL(&s_edge_lock);
    return edge;
}

bool display_te_clock_snapshot(display_te_clock_t *out) {
    if (!out) return false;
    portENTER_CRITICAL(&s_edge_lock);
    *out = (display_te_clock_t){s_accepted_edge_us, s_edge_us, s_period_us};
    portEXIT_CRITICAL(&s_edge_lock);
    return out->accepted_edge_us > 0 && out->period_us > 0;
}
