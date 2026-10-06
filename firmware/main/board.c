#include "board.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board";

i2c_master_bus_handle_t g_i2c_bus;
static i2c_master_dev_handle_t s_pmic;
static spi_device_handle_t s_lcd;
static esp_lcd_touch_handle_t s_touch;
static SemaphoreHandle_t s_lcd_slots;
// A slot owns its descriptor until the colour DMA completes. One extra
// slot lets the renderer fill its next pixel buffer without touching DMA data.
// Window commands are queued too, so a frame's windows follow one another without draining the DMA queue.
#define LCD_TRANS 8
static SemaphoreHandle_t s_lcd_free;  // unused entries of s_lcd_trans
static spi_transaction_ext_t s_lcd_trans[LCD_TRANS];
#define LCD_TRANS_PIXELS ((void *)1)
#define LCD_TRANS_COMMAND ((void *)2)
static bool s_lcd_stream, s_lcd_first, s_lcd_held;  // held: an ended window may still be in flight
static unsigned s_lcd_next;

// ---------------------------------------------------------------- AXP2101

#define AXP_ADDR 0x34

static esp_err_t axp_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_pmic, buf, 2, 100);
}

static int axp_read(uint8_t reg) {
    uint8_t val = 0;
    if (i2c_master_transmit_receive(s_pmic, &reg, 1, &val, 1, 100) != ESP_OK) return -1;
    return val;
}

static void axp_set_bits(uint8_t reg, uint8_t mask, bool on) {
    int v = axp_read(reg);
    if (v < 0) return;
    axp_write(reg, on ? (v | mask) : (v & ~mask));
}

static void pmic_init(void) {
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(g_i2c_bus, &cfg, &s_pmic));

    // PWR key, AXP2101 REG 0x27 = 0x23: IRQLEVEL 2 s, OFFLEVEL 4 s, ONLEVEL 2 s.
    // While running, a 2 s hold raises the long-press IRQ (goodbye, then software power-off).
    // A 4 s hold still cuts the rails in hardware if the firmware hangs (REG 0x22 bit 1, power-off).
    // From off, only a hold longer than 2 s turns DCDC/LDO back on. A short press does not.
    axp_write(0x22, 0b110);
    axp_write(0x27, 0x23);
    // DC1 = 3.3 V (MCU), ALDO1..4 = 3.3 V (mic bias, display power enable, panel, sensors).
    axp_write(0x80, 0x01);
    axp_write(0x82, (3300 - 1500) / 100);
    axp_write(0x92, (3300 - 500) / 100);
    axp_write(0x93, (3300 - 500) / 100);
    axp_write(0x94, (3300 - 500) / 100);
    axp_write(0x95, (3300 - 500) / 100);
    axp_write(0x90, 0x0F);
    // Charger: 4.2 V, 500 mA, 50 mA precharge, 25 mA termination.
    axp_write(0x64, 0x03);
    axp_write(0x61, 0x02);
    axp_write(0x62, 0x0B);
    axp_write(0x63, 0x01);
    // Battery detection, voltage ADC and fuel gauge.
    axp_set_bits(0x68, 0x01, true);
    axp_set_bits(0x30, 0x01, true);
    axp_set_bits(0x18, 0x08, true);
    // Only PWR key short/long press interrupts.
    axp_write(0x40, 0x00);
    axp_write(0x41, 0x0C);
    axp_write(0x42, 0x00);
    axp_write(0x48, 0xFF);
    axp_write(0x49, 0xFF);
    axp_write(0x4A, 0xFF);
}

void pmic_read(power_status_t *out) {
    memset(out, 0, sizeof(*out));
    int s1 = axp_read(0x00), s2 = axp_read(0x01);
    out->battery_pct = -1;
    out->usb_power = true;  // unknown power source: keep light sleep (and the USB console with it) off
    if (s1 < 0 || s2 < 0) return;
    out->power_known = true;
    out->usb_power = (s1 >> 5) & 1;
    out->battery_present = (s1 >> 3) & 1;
    out->charging = ((s2 >> 5) & 0x3) == 1;
    if (out->battery_present) {
        int hi = axp_read(0x34), lo = axp_read(0x35);
        if (hi >= 0 && lo >= 0) out->battery_mv = ((hi & 0x3F) << 8) | lo;
        int percent = axp_read(0xA4);
        out->battery_pct = percent >= 0 && percent <= 100 ? percent : -1;
    } else {
        out->battery_pct = -1;
    }
}

int pmic_pwr_keys(void) {
    int st = axp_read(0x49);
    if (st <= 0) return 0;
    axp_write(0x49, st);
    return st & (PMIC_KEY_SHORT | PMIC_KEY_LONG);
}

void pmic_power_off(void) { axp_set_bits(0x10, 0x01, true); }

static void aldo3(bool on) { axp_set_bits(0x90, 0x04, on); }

// ---------------------------------------------------------------- Display

static const struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t data_bytes;
    uint16_t delay_ms;
} lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0x30}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 20},
};

static void IRAM_ATTR lcd_trans_done(spi_transaction_t *t) {
    if (!t->user) return;  // polling (register) transactions
    BaseType_t woken = pdFALSE;
    if (t->user == LCD_TRANS_PIXELS) xSemaphoreGiveFromISR(s_lcd_slots, &woken);  // its buffer is free again
    xSemaphoreGiveFromISR(s_lcd_free, &woken);
    if (woken) portYIELD_FROM_ISR();
}

// Hardware command/address phases keep command + payload in a single CS pulse.
// 0x02 writes registers; 0x32 writes pixels over four data lines.
static void lcd_param(uint8_t cmd, const void *data, size_t n) {
    spi_transaction_t t = {.cmd = 0x02, .addr = (uint32_t)cmd << 8,
                           .length = n * 8, .tx_buffer = n ? data : NULL};
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_lcd, &t));
}

static void display_init(void) {
    s_lcd_slots = xSemaphoreCreateCounting(LCD_PIPE - 1, LCD_PIPE - 1);
    s_lcd_free = xSemaphoreCreateCounting(LCD_TRANS, LCD_TRANS);
    assert(s_lcd_slots && s_lcd_free);
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_CLK,
        .data0_io_num = PIN_LCD_D0,
        .data1_io_num = PIN_LCD_D1,
        .data2_io_num = PIN_LCD_D2,
        .data3_io_num = PIN_LCD_D3,
        .max_transfer_sz = LCD_W * LCD_STRIPE_MAX_ROWS * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .command_bits = 8,
        .address_bits = 24,
        .mode = 0,
        .clock_speed_hz = 40 * 1000 * 1000, // panel link: 80 MHz produced reported color/stripe artifacts
        .spics_io_num = PIN_LCD_CS,
        .queue_size = LCD_TRANS,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_RETURN_RESULT,
        .post_cb = lcd_trans_done,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_lcd));
    display_panel_init();
}

// The register sequence again, without the power cycle, sleep-out, brightness or display-on: invisible.
// The panel was seen to stop taking frames (the picture froze) until it was re-initialised.
void display_panel_refresh(void) {
    for (unsigned i = 0; i < sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]); i++) {
        uint8_t c = lcd_init_cmds[i].cmd;
        if (c == 0x11 || c == 0x51 || c == 0x29) continue;
        lcd_param(c, lcd_init_cmds[i].data, lcd_init_cmds[i].data_bytes);
    }
}

// Panel power cycle, register sequence and a black frame. Also recovers a panel that stopped taking frames.
void display_panel_init(void) {
    // Preserve the manufacturer's panel power and register sequence.
    aldo3(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    aldo3(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    aldo3(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    for (unsigned i = 0; i < sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]); i++) {
        lcd_param(lcd_init_cmds[i].cmd, lcd_init_cmds[i].data, lcd_init_cmds[i].data_bytes);
        if (lcd_init_cmds[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(lcd_init_cmds[i].delay_ms));
    }
    // Clear panel RAM before increasing brightness.
    uint16_t *black = heap_caps_calloc(LCD_W * 8, sizeof(*black), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(black ? ESP_OK : ESP_ERR_NO_MEM);
    display_begin(0, 0, LCD_W, LCD_H);
    for (int y = 0; y < LCD_H; y += 8) display_write(black, LCD_W * 8, y + 8 == LCD_H);
    display_end();
    display_wait_idle(); // every DMA transaction is done before its source is freed
    free(black);

}

void display_wait_idle(void) {
    for (int i = 0; i < LCD_TRANS; i++) xSemaphoreTake(s_lcd_free, portMAX_DELAY);
    for (int i = 0; i < LCD_TRANS; i++) xSemaphoreGive(s_lcd_free);
    if (s_lcd_held) {
        spi_device_release_bus(s_lcd);
        s_lcd_held = false;
    } else if (!s_lcd_stream) {
        ESP_ERROR_CHECK(spi_device_acquire_bus(s_lcd, portMAX_DELAY));
        spi_device_release_bus(s_lcd);
    }
}

static spi_transaction_ext_t *lcd_trans_next(void) {
    xSemaphoreTake(s_lcd_free, portMAX_DELAY);
    spi_transaction_ext_t *t = &s_lcd_trans[s_lcd_next];
    s_lcd_next = (s_lcd_next + 1) % LCD_TRANS;
    return t;
}

static void lcd_queue_param(uint8_t cmd, int a, int b) {
    spi_transaction_ext_t *t = lcd_trans_next();
    *t = (spi_transaction_ext_t){.base = {.cmd = 0x02, .addr = (uint32_t)cmd << 8, .length = 32,
                                          .flags = SPI_TRANS_USE_TXDATA, .user = LCD_TRANS_COMMAND,
                                          .tx_data = {a >> 8, a, b >> 8, b}}};
    ESP_ERROR_CHECK(spi_device_queue_trans(s_lcd, &t->base, portMAX_DELAY));
}

void display_begin(int x0, int y0, int x1, int y1) {
    assert(!s_lcd_stream && x0 >= 0 && x1 <= LCD_W && y0 >= 0 && y1 <= LCD_H && x1 > x0 && y1 > y0);
    // The previous window may still be sending: this one queues behind it.
    if (!s_lcd_held) {
        ESP_ERROR_CHECK(spi_device_acquire_bus(s_lcd, portMAX_DELAY));
        s_lcd_held = true;
    }
    s_lcd_stream = s_lcd_first = true;
    lcd_queue_param(0x2A, x0, x1 - 1);
    lcd_queue_param(0x2B, y0, y1 - 1);
}

void display_write(const uint16_t *pixels, int count, bool last) {
    assert(s_lcd_stream && count > 0 && count <= LCD_W * LCD_STRIPE_MAX_ROWS);
    xSemaphoreTake(s_lcd_slots, portMAX_DELAY);
    spi_transaction_ext_t *t = lcd_trans_next();
    *t = (spi_transaction_ext_t){
        .base = {.cmd = 0x32, .addr = 0x2C00, .length = count * 16, .tx_buffer = pixels,
                 .flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR |
                          (last ? 0 : SPI_TRANS_CS_KEEP_ACTIVE), .user = LCD_TRANS_PIXELS},
        .command_bits = s_lcd_first ? 8 : 0, .address_bits = s_lcd_first ? 24 : 0};
    s_lcd_first = false;
    ESP_ERROR_CHECK(spi_device_queue_trans(s_lcd, &t->base, portMAX_DELAY));
}

void display_end(void) {
    assert(s_lcd_stream);
    // The tail keeps going while the next window or frame is prepared; display_wait_idle() waits for it.
    s_lcd_stream = false;
}

void display_set_brightness(uint8_t level) { lcd_param(0x51, &level, 1); }
void display_power(bool on) { lcd_param(on ? 0x29 : 0x28, NULL, 0); }


// ---------------------------------------------------------------- Touch

static void touch_init(void) {
    esp_lcd_touch_config_t cfg = {
        .x_max = LCD_W,
        .y_max = LCD_H,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 1, .mirror_x = 0, .mirror_y = 1},
    };
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(g_i2c_bus, &io_cfg, &io) != ESP_OK ||
        esp_lcd_touch_new_i2c_cst9217(io, &cfg, &s_touch) != ESP_OK) {
        ESP_LOGW(TAG, "touch controller not available");
        s_touch = NULL;
    }
}

bool touch_read(int *x, int *y) {
    if (!s_touch) return false;
    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) return false;
    esp_lcd_touch_point_data_t pt[1];
    uint8_t n = 0;
    if (esp_lcd_touch_get_data(s_touch, pt, &n, 1) != ESP_OK || n == 0) return false;
    *x = pt[0].x;
    *y = pt[0].y;
    return true;
}

// ---------------------------------------------------------------- Init

void board_init(void) {
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &g_i2c_bus));
    pmic_init();
    display_init();
    touch_init();

    gpio_config_t keys = {
        .pin_bit_mask = (1ULL << PIN_KEY) | (1ULL << PIN_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&keys);
    gpio_config_t pwr = {.pin_bit_mask = 1ULL << PIN_PWR, .mode = GPIO_MODE_INPUT};
    gpio_config(&pwr);
    ESP_LOGI(TAG, "board ready");
}
