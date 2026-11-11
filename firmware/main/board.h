// Waveshare C6 Touch-AMOLED-2.16 and S3 Touch-AMOLED-1.8 V2.
// S3 map: waveshare/esp32_s3_touch_amoled_1_8 BSP 2.0.3.
// C6 map: official ESP-IDF BSP and XiaoZhi v2.2.5 (the example map is stale).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "viewport.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3
#include "display_s3.h"
#define PIN_I2C_SDA 15
#define PIN_I2C_SCL 14
#define PIN_LCD_CLK 11
#define PIN_LCD_D0 4
#define PIN_LCD_D1 5
#define PIN_LCD_D2 6
#define PIN_LCD_D3 7
#define PIN_LCD_CS 12
#define PIN_LCD_TE 13
#define PIN_TP_INT 21
#define PIN_TP_RST (-1)
#define PIN_I2S_MCLK 16
#define PIN_I2S_BCLK 9
#define PIN_I2S_WS 45
#define PIN_I2S_DIN 10
#define PIN_I2S_DOUT 8
#define PIN_AUDIO_PA 46
#define PIN_KEY (-1)
#define PIN_BOOT 0
#define PIN_PWR (-1)
#define PANEL_W DISPLAY_S3_WIDTH
#define PANEL_H DISPLAY_S3_HEIGHT
#define PANEL_X_GAP 16
#else

#define PIN_I2C_SDA 8
#define PIN_I2C_SCL 7

#define PIN_LCD_CLK 0
#define PIN_LCD_D0 1
#define PIN_LCD_D1 2
#define PIN_LCD_D2 3
#define PIN_LCD_D3 4
#define PIN_LCD_CS 15

#define PIN_TP_INT 5
#define PIN_TP_RST 11

#define PIN_I2S_MCLK 19
#define PIN_I2S_BCLK 20
#define PIN_I2S_WS 22
#define PIN_I2S_DIN 21   // ES7210 -> MCU
#define PIN_I2S_DOUT 23  // MCU -> ES8311

#define PIN_KEY 10   // "IO10" key, active low: push-to-talk
#define PIN_BOOT 9   // BOOT key, active low (strapping pin)
#define PIN_PWR 18   // PWR key via inverter, active high; AXP2101 PWRON
#define PIN_AUDIO_PA (-1)
#define PANEL_W 480
#define PANEL_H 480
#define PANEL_X_GAP 0
#endif

#define LCD_W UI_RENDER_W
#define LCD_H UI_RENDER_H
#define LCD_STRIPE_MAX_ROWS 24

extern i2c_master_bus_handle_t g_i2c_bus;

void board_init(void);
bool board_key_down(void);

// Native panel transport, used by the S3 portrait adapter.
void board_display_begin(int x0, int y0, int x1, int y1);
void board_display_write(const uint16_t *pixels, int count, bool last);
void board_display_end(void);
void board_display_wait_idle(void);
#if CONFIG_IDF_TARGET_ESP32S3
// Arm once before a frame's first window, after the preceding frame drained.
// Called in SPI ISR context immediately before its first pixel transaction.
void board_display_frame_start(void (*started)(void *), void *context);
#endif

// RGB565 QSPI windows, with CS held between DMA chunks inside each window.
// C6 reserves one credit before writing a rotating DMA buffer; completion returns it.
// S3 takes credits when queueing. end() keeps tails owned; wait_idle() drains them.
#if CONFIG_IDF_TARGET_ESP32S3
#define LCD_PIPE 3
#define LCD_DMA_CREDITS (LCD_PIPE - 1)
#else
#define LCD_PIPE 2
#define LCD_DMA_CREDITS LCD_PIPE
#endif
void display_begin(int x0, int y0, int x1, int y1);
#if !CONFIG_IDF_TARGET_ESP32S3
// Exactly one reservation must be consumed by one write before end()/wait_idle().
void display_reserve_buffer(void);
void display_write(const uint16_t *pixels, int count, bool last);
#endif
void display_end(void);
void display_wait_idle(void);
void display_set_brightness(uint8_t level_0_255);
void display_power(bool on);
void display_panel_init(void);     // power-cycle and re-initialise the panel (between frames only)
void display_panel_refresh(void);  // re-send the configuration registers (between frames only)

// PMIC
typedef struct {
    int battery_mv;
    int battery_pct;
    bool charging;
    bool usb_power;
    bool power_known;  // status registers were read successfully
    bool battery_present;
} power_status_t;
void pmic_read(power_status_t *out);
void pmic_power_off(void);
// PWR key presses since the last call (AXP2101 IRQ): PMIC_KEY_SHORT and/or PMIC_KEY_LONG (held 2 s).
#define PMIC_KEY_SHORT 0x08
#define PMIC_KEY_LONG 0x04
int pmic_pwr_keys(void);

#include "touch_input.h"
// A complete sample, including release. False means a controller/I2C error.
bool touch_read(touch_frame_t *frame);
