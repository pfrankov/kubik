// Actual pinned SDK read/get paths, with only I2C and RTOS boundaries replaced.
#include "touch_report.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_RESPONSE -2
#define ESP_ERR_INVALID_ARG -3
#define GPIO_NUM_NC -1
#define CST9217_DATA_LENGTH CST9217_REPORT_BYTES
#define CST9217_ACK_VALUE 0xab
#define ESP_LCD_TOUCH_CST9217_DATA_REG 0xd000
#define TAG "test"
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define ESP_GOTO_ON_ERROR(call, label, ...) do { ret = (call); if (ret != ESP_OK) goto label; } while (0)
typedef struct {
    int io;
    struct { int rst_gpio_num; } config;
    struct {
        int lock; uint8_t points;
        struct { uint16_t x, y; uint8_t track_id; } coords[TOUCH_POINTS];
    } data;
} esp_lcd_touch_t;
typedef esp_lcd_touch_t *esp_lcd_touch_handle_t;
static uint8_t bytes[CST9217_REPORT_BYTES];
static int read_error, ack_error, acknowledgements, resets, delay_ms;
static esp_err_t cst9217_read_reg(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, size_t len) {
    assert(tp && reg == 0xd000 && len == CST9217_REPORT_BYTES);
    if (read_error) return read_error;
    memcpy(data, bytes, len); return ESP_OK;
}
static esp_err_t esp_lcd_panel_io_tx_param(int io, int command, const void *data, size_t len) {
    const uint8_t *p = data;
    assert(io == 7 && command == 0xd0 && len == 2 && p[0] == 0x00 && p[1] == 0xab);
    acknowledgements++; return ack_error;
}
static void gpio_set_level(int pin, int level) { assert(pin == 3 && (level == 0 || level == 1)); resets++; }
static void vTaskDelay(int delay) { delay_ms += delay; }

/* READ_DATA */
/* GET_XY */
/* GET_TRACK_ID */

int main(void) {
    esp_lcd_touch_t tp = {.io = 7, .config.rst_gpio_num = 3};
    bytes[6] = 0xab;
    assert(esp_lcd_touch_cst9217_read_data(&tp) == ESP_OK); assert(tp.data.points == 0); assert(acknowledgements == 1);
    bytes[5] = 1; bytes[0] = 0xe6; bytes[1] = 0x1d; bytes[3] = 0xf0;
    assert(esp_lcd_touch_cst9217_read_data(&tp) == ESP_OK); assert(tp.data.points == 1); assert(acknowledgements == 2);
    assert(tp.data.coords[0].x == 479); assert(tp.data.coords[0].track_id == 14);
    bytes[5] = 2;
    bytes[0] = 0xf6; bytes[1] = 0x10; bytes[2] = 0x12; bytes[3] = 0x34;
    bytes[7] = 0x16; bytes[8] = 0x02; bytes[9] = 0x04; bytes[10] = 0x56;
    assert(esp_lcd_touch_cst9217_read_data(&tp) == ESP_OK); assert(tp.data.points == 2); assert(acknowledgements == 3);
    uint16_t x[2], y[2], strength[2]; uint8_t n, ids[2];
    assert(esp_lcd_touch_cst9217_get_xy(&tp, x, y, strength, &n, 2)); assert(n == 2);
    assert(x[0] == 259); assert(y[0] == 292); assert(x[1] == 37); assert(y[1] == 70);
    assert(cst9217_get_track_id(&tp, ids, n) == ESP_OK); assert(ids[0] == 15); assert(ids[1] == 1);
    assert(cst9217_get_track_id(&tp, ids, 3) == ESP_ERR_INVALID_ARG);
    bytes[0] = 0xf0; // lifted first record cannot hide the held second contact
    assert(esp_lcd_touch_cst9217_read_data(&tp) == ESP_OK); assert(tp.data.points == 1);
    assert(tp.data.coords[0].track_id == 1); assert(tp.data.coords[0].x == 37);
    bytes[6] = 0;
    assert(esp_lcd_touch_cst9217_read_data(&tp) == ESP_ERR_INVALID_RESPONSE); assert(!tp.data.points);
    assert(acknowledgements == 5); assert(!resets); // malformed received data must still be ACKed
    bytes[6] = 0xab; read_error = -8; tp.data.points = 2;
    assert(esp_lcd_touch_cst9217_read_data(&tp) == ESP_FAIL); assert(!tp.data.points); assert(acknowledgements == 5);
    assert(resets == 2); assert(delay_ms == 60);
    read_error = 0; ack_error = -9; tp.data.points = 1;
    assert(esp_lcd_touch_cst9217_read_data(&tp) == ESP_FAIL); assert(!tp.data.points); assert(acknowledgements == 6);
    assert(resets == 4); assert(delay_ms == 120);
    ack_error = 0; bytes[5] = 0;
    assert(esp_lcd_touch_cst9217_read_data(&tp) == ESP_OK); assert(!tp.data.points);
    puts("CST9217: actual SDK read/ACK/getter, independent IDs, compaction, malformed reports and I2C errors passed");
}
