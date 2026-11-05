#!/usr/bin/env python3
"""Run actual board initializers for both targets with recorded hardware calls."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
BOARD = (ROOT / 'firmware/main/board.c').read_text()
HEADER = (ROOT / 'firmware/main/board.h').read_text()
AUDIO = (ROOT / 'firmware/main/audio_board.c').read_text()


def function(source, declaration):
    start = source.index(declaration)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


STUB = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERROR_CHECK(value) do { if ((value) != ESP_OK) longjmp(failure, 1); } while (0)
static jmp_buf failure;
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(value) (value)
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define I2C_ADDR_BIT_LEN_7 7
#define I2C_NUM_0 0
#define I2C_CLK_SRC_DEFAULT 0
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define GPIO_PULLUP_ENABLE 1
#define AXP_ADDR 0x34
#define CHARACTER_S3 CONFIG_IDF_TARGET_ESP32S3
static int g_i2c_bus, s_pmic;
static void *s_touch;
static void touch_init(void);
void board_init(void);
bool board_key_down(void);
typedef int i2c_master_dev_handle_t;
static int registers[256], commands[256], rail_toggles;
static uint8_t expander_values[8], expander_count, expander_config, expander_input = 0xff;
static int delays[64], delay_count, clears, touch_failure;
static uint64_t input_pins;
typedef struct {int dev_addr_length, device_address, scl_speed_hz;} i2c_device_config_t;
typedef struct {int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
                struct {int enable_internal_pullup;} flags;} i2c_master_bus_config_t;
typedef struct {uint64_t pin_bit_mask; int mode, pull_up_en;} gpio_config_t;
typedef struct {int x_max, y_max, rst_gpio_num, int_gpio_num;
                struct {int reset, interrupt;} levels;
                struct {int swap_xy, mirror_x, mirror_y;} flags;} esp_lcd_touch_config_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef struct {int scl_speed_hz;} esp_lcd_panel_io_i2c_config_t;
#define ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG() {0}
#define ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG() {0}
static esp_lcd_touch_config_t touch_config;
static int i2c_master_bus_add_device(int bus, const i2c_device_config_t *cfg, int *device) {
    assert(bus == 1 && cfg->scl_speed_hz == 400000);
    assert(cfg->device_address == 0x34 || (CHARACTER_S3 && cfg->device_address == 0x20));
    *device = cfg->device_address == 0x34 ? 2 : 3; return 0;
}
static int i2c_new_master_bus(const i2c_master_bus_config_t *cfg, int *bus) {
    assert(cfg->sda_io_num == PIN_I2C_SDA && cfg->scl_io_num == PIN_I2C_SCL);
    *bus = 1; return 0;
}
static void axp_write(uint8_t reg, uint8_t value) { registers[reg] = value; }
static void axp_set_bits(uint8_t reg, uint8_t mask, bool on) {
    registers[reg] = on ? registers[reg] | mask : registers[reg] & ~mask;
}
static void aldo3(bool on) { (void)on; rail_toggles++; }
static int i2c_master_transmit(int device, const uint8_t *data, size_t bytes, int timeout) {
    assert(device == 3 && bytes == 2 && timeout == 100);
    if (data[0] == 3) expander_config = data[1];
    else { assert(data[0] == 1 && expander_count < 8); expander_values[expander_count++] = data[1]; }
    return 0;
}
static int gpio_get_level(int pin) { assert(pin == PIN_KEY); return expander_input & 0x10; }
static int gpio_config(const gpio_config_t *cfg) {
    if (cfg->mode == GPIO_MODE_INPUT) input_pins |= cfg->pin_bit_mask;
    else assert(false);
    return 0;
}
bool display_s3_init(void) { return true; }
static int display_te_init(void) { return 0; }
static void vTaskDelay(int ms) { assert(delay_count < 64); delays[delay_count++] = ms; }
static void lcd_param(uint8_t command, const void *data, size_t bytes) {
    commands[command] = bytes ? *(const uint8_t *)data : 0x100;
}
static void *heap_caps_calloc(size_t count, size_t size, int caps) {
    assert(caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)); return calloc(count, size);
}
static void board_display_begin(int x, int y, int x1, int y1) {
    assert(x == 0 && y == 0 && x1 == PANEL_W && y1 == PANEL_H);
}
static void board_display_write(const uint16_t *pixels, int count, bool last) {
    assert(count == PANEL_W * 8 && last == (clears == PANEL_H / 8 - 1));
    for (int i = 0; i < count; i++) assert(pixels[i] == 0);
    clears++;
}
static void board_display_end(void) { assert(clears == PANEL_H / 8); }
static void board_display_wait_idle(void) { assert(clears == PANEL_H / 8); }
static int esp_lcd_new_panel_io_i2c(int bus, const esp_lcd_panel_io_i2c_config_t *cfg, void **io) {
    assert(bus == 1 && cfg->scl_speed_hz == 400000); *io = (void *)3;
    return touch_failure == 1 ? ESP_FAIL : ESP_OK;
}
static int esp_lcd_touch_new_i2c_cst9217(void *io, const esp_lcd_touch_config_t *cfg, void **touch) {
    assert(io == (void *)3); touch_config = *cfg; *touch = (void *)4;
    return touch_failure == 2 ? ESP_FAIL : ESP_OK;
}
static int esp_lcd_touch_new_i2c_cst816s(void *io, const esp_lcd_touch_config_t *cfg, void **touch) {
    return esp_lcd_touch_new_i2c_cst9217(io, cfg, touch);
}
// The real display initializer's transport is independently checked by firmware builds.
static void display_panel_init(void);
static void display_init(void) { display_panel_init(); }
typedef struct {double pa_voltage, codec_dac_voltage;} gain_t;
typedef struct {void *ctrl_if, *gpio_if; int codec_mode, pa_pin; bool use_mclk; gain_t hw_gain;} es8311_codec_cfg_t;
#define ESP_CODEC_DEV_WORK_MODE_DAC 1
#define ESP_CODEC_DEV_WORK_MODE_BOTH 3
static void check_codec(void) {
    void *out_ctrl = NULL, *gpio_if = NULL;
'''

TEST = r'''
    assert(es8311_cfg.pa_pin == (CHARACTER_S3 ? 46 : -1));
    assert(es8311_cfg.use_mclk && es8311_cfg.codec_mode == (CHARACTER_S3 ? ESP_CODEC_DEV_WORK_MODE_BOTH : ESP_CODEC_DEV_WORK_MODE_DAC));
}
int main(void) {
    const int s3[] = {15,14,11,4,5,6,7,12,21,-1,16,9,45,10,8,-1,0,-1};
    const int c6[] = {8,7,0,1,2,3,4,15,5,11,19,20,22,21,23,10,9,18};
    const int actual[] = {PIN_I2C_SDA,PIN_I2C_SCL,PIN_LCD_CLK,PIN_LCD_D0,PIN_LCD_D1,
        PIN_LCD_D2,PIN_LCD_D3,PIN_LCD_CS,PIN_TP_INT,PIN_TP_RST,PIN_I2S_MCLK,
        PIN_I2S_BCLK,PIN_I2S_WS,PIN_I2S_DIN,PIN_I2S_DOUT,PIN_KEY,PIN_BOOT,PIN_PWR};
    assert(!memcmp(actual, CHARACTER_S3 ? s3 : c6, sizeof actual));
    if (setjmp(failure)) assert(false && "unexpected hardware error");
    board_init();
#if CONFIG_IDF_TARGET_ESP32S3
    assert(input_pins == (1ULL << PIN_BOOT));
    assert(expander_config == 0x78 && expander_count == 4);
    const uint8_t expected[] = {0x80,0x87,0x86,0x87};
    assert(!memcmp(expander_values, expected, sizeof expected));
#else
    assert(input_pins == ((1ULL << PIN_KEY) | (1ULL << PIN_BOOT) | (1ULL << PIN_PWR)));
#endif
    assert(!board_key_down()); expander_input = 0xef;
    assert(board_key_down() == !CHARACTER_S3); // EXIO4 is PWR sense on S3, never a phantom KEY.
    assert(registers[0x80] == 1 && registers[0x82] == 18 && registers[0x92] == 28);
    assert(registers[0x90] == (CHARACTER_S3 ? 1 : 15));
    assert(registers[0x64] == (CHARACTER_S3 ? 2 : 3));
    assert(registers[0x62] == (CHARACTER_S3 ? 8 : 11));
    assert(registers[0x22] == 6 && registers[0x27] == 0x23 && registers[0x41] == 0x0c);
    assert(commands[0x36] == (CHARACTER_S3 ? 0 : 0x30));
    assert(commands[0x19] == (CHARACTER_S3 ? 0 : 0x10));
    assert(commands[0x1c] == (CHARACTER_S3 ? 0 : 0xa0));
    assert(commands[0x3a] == 0x55 && commands[0xc4] == 0x80 && commands[0x29] == 0x100);
    assert(rail_toggles == (CHARACTER_S3 ? 0 : 3));
    if (CHARACTER_S3) assert(delays[0] == 20 && delays[1] == 150 && delays[2] == 20 && delays[3] == 150);
    else assert(delays[0] == 50 && delays[1] == 100 && delays[2] == 100);
    assert(s_touch == (void *)4 && touch_config.flags.swap_xy == !CHARACTER_S3 && !touch_config.flags.mirror_x && touch_config.flags.mirror_y == !CHARACTER_S3);
    assert(touch_config.rst_gpio_num == PIN_TP_RST && touch_config.int_gpio_num == PIN_TP_INT);
    for (touch_failure = 1; touch_failure <= 2; touch_failure++) {
#if CONFIG_IDF_TARGET_ESP32S3
        if (!setjmp(failure)) { touch_init(); assert(false && "mandatory touch failure was hidden"); }
#else
        touch_init(); assert(s_touch == NULL);
#endif
    }
    check_codec();
}
'''

pins = HEADER[HEADER.index('#if CONFIG_IDF_TARGET_ESP32S3'):HEADER.index('extern i2c_master_bus_handle_t')]
table = BOARD[BOARD.index('static const struct {'):BOARD.index('static void IRAM_ATTR lcd_trans_done(')]
codec_start = AUDIO.index('    es8311_codec_cfg_t es8311_cfg = {')
codec = AUDIO[codec_start:AUDIO.index('    };', codec_start) + 7]
expander = BOARD[BOARD.index('#if CONFIG_IDF_TARGET_ESP32S3\nstatic i2c_master_dev_handle_t s_expander;'):BOARD.index('// ---------------------------------------------------------------- AXP2101')]
production = expander + function(BOARD, 'static void pmic_init(') + table
production += function(BOARD, 'void display_panel_init(')
production += function(BOARD, 'static void touch_init(') + function(BOARD, 'void board_init(')
with tempfile.TemporaryDirectory(prefix='kubik-board-') as temporary:
    directory = Path(temporary)
    source = directory / 'board.c'
    source.write_text(pins + STUB + codec + TEST + production)
    for s3 in (0, 1):
        executable = directory / f'board-{s3}'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra',
                        '-Wno-unused-function', '-fsanitize=address,undefined',
                        f'-DCONFIG_IDF_TARGET_ESP32S3={s3}', '-I' + str(ROOT / 'firmware/main'), str(source), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
print('board: C6/S3 pins, PMIC rails/charger, panel reset/init/clear, touch failure and amplifier routing passed')

# Exercise independent ES8311 ADC power and the shared speaker wake path.
gate = AUDIO[AUDIO.index('static const audio_codec_if_t *s_shared_codec;'):AUDIO.index('// audio.c uses this interface')]
gate_stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define ESP_CODEC_DEV_OK 0
#define portMAX_DELAY 0
typedef int SemaphoreHandle_t;
typedef struct codec audio_codec_if_t;
struct codec {
    int (*get_reg)(const audio_codec_if_t *, int, int *);
    int (*set_reg)(const audio_codec_if_t *, int, int);
    int (*enable)(const audio_codec_if_t *, bool);
};
static int locked, value = 2, fail_read, fail_write, power_calls;
static bool powered = true;
static void xSemaphoreTake(int mutex, int wait) { (void)mutex; (void)wait; assert(!locked); locked = 1; }
static void xSemaphoreGive(int mutex) { (void)mutex; assert(locked); locked = 0; }
static int read_reg(const audio_codec_if_t *codec, int reg, int *out) {
    (void)codec; assert(reg == 0x0e); *out = value; return fail_read ? -1 : 0;
}
static int write_reg(const audio_codec_if_t *codec, int reg, int next) {
    (void)codec; assert(reg == 0x0e); if (fail_write) return -1; value = next; return 0;
}
static int enable(const audio_codec_if_t *codec, bool on) {
    (void)codec; powered = on; power_calls++; if (on) value = 2; return 0;
}
'''
gate_test = r'''
int main(void) {
    const audio_codec_if_t codec = {.get_reg=read_reg, .set_reg=write_reg, .enable=enable};
    s_shared_codec = &codec;
    assert(audio_board_adc_enable(false) == 0 && value == 0x62 && powered && !s_adc_on);
    assert(audio_board_adc_enable(true) == 0 && value == 2 && powered && s_adc_on);
    assert(power_calls == 0); // Stopping/starting the microphone never powers off DAC/PA.
    value = 0x8a;
    assert(audio_board_adc_enable(false) == 0 && value == 0xea); // Preserve all other control bits.
    assert(shared_output_power(NULL, false) == 0 && !powered);
    assert(shared_output_power(NULL, true) == 0 && powered && value == 0x62 && !s_adc_on);
    assert(audio_board_adc_enable(true) == 0 && value == 2);
    assert(shared_output_power(NULL, false) == 0);
    assert(shared_output_power(NULL, true) == 0 && value == 2 && s_adc_on);
    fail_write = 1;
    assert(audio_board_adc_enable(false) != 0 && s_adc_on);
    fail_write = 0; fail_read = 1;
    assert(audio_board_adc_enable(false) != 0 && s_adc_on);
    fail_read = 0;
    assert(audio_board_adc_enable(false) == 0 && !s_adc_on && value == 0x62);
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-shared-codec-') as temporary:
    source, executable = Path(temporary) / 'gate.c', Path(temporary) / 'gate'
    source.write_text(gate_stub + gate + gate_test)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('ES8311: independent ADC power, preserved DAC, wake re-gating and failed-register state passed')

# Exercise the actual SPI callbacks and the UI/worker idle ownership boundary.
callback_start = BOARD.index('static portMUX_TYPE s_lcd_start_lock')
callbacks = BOARD[callback_start:BOARD.index('#endif', callback_start)]
dev_start = BOARD.index('    spi_device_interface_config_t dev = {')
device = BOARD[dev_start:BOARD.index('    ESP_ERROR_CHECK(spi_bus_add_device(', dev_start)]
spi_stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define IRAM_ATTR
#define pdFALSE 0
#define portMAX_DELAY 99
#define portMUX_INITIALIZER_UNLOCKED 0
#define LCD_TRANS 8
#define LCD_TRANS_PIXELS ((void *)1)
#define LCD_TRANS_COMMAND ((void *)2)
#define SPI_DEVICE_HALFDUPLEX 1
#define SPI_DEVICE_NO_RETURN_RESULT 2
#define PIN_LCD_CS (CONFIG_IDF_TARGET_ESP32S3 ? 12 : 15)
#define ESP_ERROR_CHECK(value) assert((value)==0)
typedef int portMUX_TYPE,BaseType_t;
typedef struct {void *user;} spi_transaction_t;
typedef struct {
    int command_bits,address_bits,mode,clock_speed_hz,spics_io_num,queue_size,flags;
    void (*pre_cb)(spi_transaction_t *),(*post_cb)(spi_transaction_t *);
} spi_device_interface_config_t;
static bool s_lcd_stream,s_lcd_held,worker_pending,worker_context,isr;
static int s_lcd=1,s_lcd_slots=2,s_lcd_free=3;
static int critical_depth,starts,context_value,slot_gives,free_gives;
static int raw_takes,fences,acquires,releases,worker_releases;
#define portENTER_CRITICAL(lock) do {(void)(lock);assert(!critical_depth++);} while(0)
#define portEXIT_CRITICAL(lock) do {(void)(lock);assert(critical_depth--==1);} while(0)
#define portENTER_CRITICAL_ISR(lock) portENTER_CRITICAL(lock)
#define portEXIT_CRITICAL_ISR(lock) portEXIT_CRITICAL(lock)
#define portYIELD_FROM_ISR() ((void)0)
static void xSemaphoreGiveFromISR(int semaphore,BaseType_t *woken) {
    assert(isr);if(semaphore==s_lcd_slots)slot_gives++;
    else {assert(semaphore==s_lcd_free);free_gives++;}*woken=1;
}
static void xSemaphoreTake(int semaphore,int wait) {
    assert(semaphore==s_lcd_free && wait==portMAX_DELAY);
    assert(!worker_pending || worker_context);raw_takes++;
}
static void xSemaphoreGive(int semaphore) {assert(semaphore==s_lcd_free);}
static int spi_device_acquire_bus(int lcd,int wait) {
    assert(lcd==s_lcd && wait==portMAX_DELAY);acquires++;return 0;
}
static void spi_device_release_bus(int lcd) {
    assert(lcd==s_lcd);releases++;
    if(worker_context)worker_releases++;
}
void board_display_wait_idle(void);
static void display_s3_tx_wait_idle(void) {
    assert(!raw_takes && worker_pending && s_lcd_held);fences++;
    // A delayed worker finishes DMA and releases its own bus before UI continues.
    worker_context=true;board_display_wait_idle();worker_context=false;worker_pending=false;
}
static void started(void *context) {
    assert(isr && !critical_depth);starts++;context_value=*(int *)context;
}
'''
spi_test = r'''
static void check_callbacks(void) {
@DEVICE@
    assert(dev.clock_speed_hz==40000000 && dev.post_cb==lcd_trans_done);
    spi_transaction_t pixel={LCD_TRANS_PIXELS},command={LCD_TRANS_COMMAND},polling={NULL};
#if CONFIG_IDF_TARGET_ESP32S3
    assert(dev.pre_cb==lcd_trans_start);
    int first=17,second=29;
    board_display_frame_start(started,&first);isr=true;
    dev.pre_cb(&polling);dev.pre_cb(&command);assert(!starts);
    dev.pre_cb(&pixel);assert(starts==1 && context_value==17);
    dev.pre_cb(&pixel);dev.pre_cb(&command);assert(starts==1);isr=false;
    board_display_frame_start(started,&second);isr=true;
    dev.pre_cb(&command);dev.pre_cb(&pixel);dev.pre_cb(&pixel);
    assert(starts==2 && context_value==29 && !s_lcd_started);isr=false;
#else
    assert(!dev.pre_cb && !starts);
#endif
    isr=true;dev.post_cb(&polling);assert(!slot_gives && !free_gives);
    dev.post_cb(&command);assert(!slot_gives && free_gives==1);
    dev.post_cb(&pixel);assert(slot_gives==1 && free_gives==2);isr=false;
}
int main(void) {
    check_callbacks();s_lcd_held=true;
    worker_pending=CONFIG_IDF_TARGET_ESP32S3;
    display_wait_idle();
    assert(!s_lcd_held && !worker_pending && !critical_depth);
    assert(fences==CONFIG_IDF_TARGET_ESP32S3 && worker_releases==CONFIG_IDF_TARGET_ESP32S3);
    assert(raw_takes==LCD_TRANS*(1+CONFIG_IDF_TARGET_ESP32S3));
    assert(acquires==CONFIG_IDF_TARGET_ESP32S3 && releases==1+CONFIG_IDF_TARGET_ESP32S3);
}
'''.replace('@DEVICE@', device)
spi_source = spi_stub + '\n#if CONFIG_IDF_TARGET_ESP32S3\n' + callbacks + '\n#endif\n'
spi_source += function(BOARD, 'static void IRAM_ATTR lcd_trans_done(')
spi_source += function(BOARD, 'void board_display_wait_idle(') + function(BOARD, 'void display_wait_idle(')
with tempfile.TemporaryDirectory(prefix='kubik-board-spi-') as temporary:
    source, executable = Path(temporary) / 'spi.c', Path(temporary) / 'spi'
    source.write_text(spi_source + spi_test)
    for s3 in (0, 1):
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-function', '-Wno-unused-variable', '-fsanitize=address,undefined',
                        f'-DCONFIG_IDF_TARGET_ESP32S3={s3}', str(source), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
print('SPI: actual first-pixel ISR callback, command exclusion, one-shot rearm, unchanged DMA credits and worker idle fencing passed')
