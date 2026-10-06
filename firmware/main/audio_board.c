#include "audio_board.h"
#include "audio.h"
#include "audio_mic_io.h"
#include "audio_output_tail.h"
#include "board.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

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
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {.port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = g_i2c_bus};
    const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = out_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *out_codec = es8311_codec_new(&es8311_cfg);
    esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = out_codec, .data_if = data_if};
    *output = esp_codec_dev_new(&dev_cfg);
    *out_codec_result = out_codec;
    i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = in_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *in_codec = es7210_codec_new(&es7210_cfg);
    *tx_result = tx;
    if (!*output || !in_codec) {
        ESP_LOGE("audio", "codec init failed");
        return false;
    }
    esp_codec_dev_sample_info_t fs_in = {
        .bits_per_sample = 16,
        .channel = 4,
        .channel_mask = 0xF,
        .sample_rate = AUDIO_RATE,
    };
    esp_codec_dev_sample_info_t fs_out = {.bits_per_sample = 16, .channel = 1, .sample_rate = AUDIO_RATE};
    ESP_ERROR_CHECK(esp_codec_dev_open(*output, &fs_out));
    ESP_ERROR_CHECK(audio_mic_io_init(capture, data_if, in_codec, rx, &fs_in) ? ESP_OK : ESP_FAIL);
    return true;
}
