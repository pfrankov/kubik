#include "audio_board.h"
#include "audio.h"
#include "audio_target.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "audio_mic_io.h"
#include "audio_output_tail.h"
#include "board.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

#if CONFIG_IDF_TARGET_ESP32S3
static const audio_codec_if_t *s_shared_codec;
static SemaphoreHandle_t s_codec_lock;
static bool s_adc_on;

// ES8311 DS REG0E bits6/5 power down only the microphone PGA/ADC modulator.
// The SDK's full start enables both paths; always reapply the capture gate.
static int shared_adc_gate(bool enabled) {
    int value = 0;
    int result = s_shared_codec->get_reg(s_shared_codec, 0x0e, &value);
    if (result != ESP_CODEC_DEV_OK) return result;
    value = enabled ? value & ~0x60 : value | 0x60;
    return s_shared_codec->set_reg(s_shared_codec, 0x0e, value);
}
int audio_board_adc_enable(bool enabled) {
    xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    int result = shared_adc_gate(enabled);
    if (result == ESP_CODEC_DEV_OK) s_adc_on = enabled;
    xSemaphoreGive(s_codec_lock);
    return result;
}
static int shared_output_power(const audio_codec_if_t *unused, bool enabled) {
    (void)unused;
    xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    int result = s_shared_codec->enable(s_shared_codec, enabled);
    if (result == ESP_CODEC_DEV_OK && enabled) result = shared_adc_gate(s_adc_on);
    xSemaphoreGive(s_codec_lock);
    return result;
}
// audio.c uses this interface only for output sleep/wake; the actual codec
// remains the authoritative format, volume and gain interface for both paths.
static const audio_codec_if_t s_output_power = {.enable = shared_output_power};
#endif

bool audio_board_init(mic_capture_t *capture, esp_codec_dev_handle_t *output,
                      const audio_codec_if_t **out_codec_result, i2s_chan_handle_t *tx_result) {
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = AUDIO_TX_DESCRIPTORS;
    chan_cfg.dma_frame_num = AUDIO_TX_DESCRIPTOR_FRAMES;
    chan_cfg.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx, &rx));
    i2s_std_config_t std_cfg = {
        .clk_cfg = {.sample_rate_hz = AUDIO_RATE, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = {.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                     .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                     .slot_mode = I2S_SLOT_MODE_STEREO,
                     .slot_mask = I2S_STD_SLOT_BOTH,
                     .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
                     .ws_pol = false,
                     .bit_shift = true,
                     .left_align = true},
        .gpio_cfg = {.mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS, .dout = PIN_I2S_DOUT,
                     .din = I2S_GPIO_UNUSED},
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std_cfg));
#if CONFIG_IDF_TARGET_ESP32S3
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = PIN_I2S_DIN;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
#else
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {.sample_rate_hz = AUDIO_RATE,
                    .clk_src = I2S_CLK_SRC_DEFAULT,
                    .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                    .bclk_div = 8},
        .slot_cfg = {.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                     .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                     .slot_mode = I2S_SLOT_MODE_STEREO,
                     .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
                     .ws_width = I2S_TDM_AUTO_WS_WIDTH,
                     .ws_pol = false,
                     .bit_shift = true,
                     .left_align = false,
                     .total_slot = I2S_TDM_AUTO_SLOT_NUM},
        .gpio_cfg = {.mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS, .dout = I2S_GPIO_UNUSED,
                     .din = PIN_I2S_DIN},
    };
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
#endif
    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {.port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = g_i2c_bus};
    const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = out_ctrl,
        .gpio_if = gpio_if,
#if CONFIG_IDF_TARGET_ESP32S3
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
#else
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
#endif
        .pa_pin = PIN_AUDIO_PA,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *out_codec = es8311_codec_new(&es8311_cfg);
    esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = out_codec, .data_if = data_if};
    *output = esp_codec_dev_new(&dev_cfg);
    *out_codec_result = out_codec;
#if CONFIG_IDF_TARGET_ESP32S3
    s_codec_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_codec_lock ? ESP_OK : ESP_ERR_NO_MEM);
    s_shared_codec = out_codec;
    *out_codec_result = &s_output_power;
    const audio_codec_if_t *in_codec = out_codec;
#else
    i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = in_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *in_codec = es7210_codec_new(&es7210_cfg);
#endif
    *tx_result = tx;
    if (!*output || !in_codec) {
        ESP_LOGE("audio", "codec init failed");
        return false;
    }
    esp_codec_dev_sample_info_t fs_in = {
        .bits_per_sample = 16,
        .channel = AUDIO_CAPTURE_CHANNELS,
        .channel_mask = (1u << AUDIO_CAPTURE_CHANNELS) - 1,
        .sample_rate = AUDIO_RATE,
    };
    esp_codec_dev_sample_info_t fs_out = {.bits_per_sample = 16, .channel = 1, .sample_rate = AUDIO_RATE};
    ESP_ERROR_CHECK(esp_codec_dev_open(*output, &fs_out));
#if CONFIG_IDF_TARGET_ESP32S3
    ESP_ERROR_CHECK(audio_board_adc_enable(false));
#endif
    ESP_ERROR_CHECK(audio_mic_io_init(capture, data_if, in_codec, rx, &fs_in) ? ESP_OK : ESP_FAIL);
    return true;
}
