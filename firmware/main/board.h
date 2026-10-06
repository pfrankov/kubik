// Waveshare ESP32-C6-Touch-AMOLED-2.16: pins and low-level peripherals.
// Pin map verified against the official ESP-IDF BSP constructor defaults and
// the XiaoZhi v2.2.5 board definition (the example user_config.h is stale).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"

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

#define LCD_W 480
#define LCD_H 480
#define LCD_STRIPE_MAX_ROWS 24

extern i2c_master_bus_handle_t g_i2c_bus;

void board_init(void);

// RGB565 QSPI windows, with CS held between DMA chunks inside each window.
// At most LCD_PIPE-1 pixel buffers are in flight. end() only closes the window;
// display_wait_idle() drains every transaction before panel commands.
#define LCD_PIPE 3
void display_begin(int x0, int y0, int x1, int y1);
void display_write(const uint16_t *pixels, int count, bool last);
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

// Touch: returns true while touched, coordinates in panel pixels.
bool touch_read(int *x, int *y);
