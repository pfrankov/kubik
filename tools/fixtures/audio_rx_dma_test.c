// Compiled with actual board initializers, pinned codec reconfiguration and IDF sizing.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "audio_target.h"
#define ESP_OK 0
#define ESP_CODEC_DEV_OK 0
#define ESP_CODEC_DEV_INVALID_ARG -1
#define ESP_CODEC_DEV_WRONG_STATE -2
#define ESP_CODEC_DEV_WRITE_FAIL -3
#define ESP_CODEC_DEV_DRV_ERR -4
#define ESP_FAIL -5
#define ESP_ERR_INVALID_ARG -6
#define ESP_RETURN_ON_FALSE(condition, error, tag, ...) do { if (!(condition)) return error; } while (0)
#define ESP_RETURN_ON_ERROR(result, tag, ...) do { int error=(result); if(error) return error; } while (0)
#define ESP_LOGI(...) ((void)0)
#define I2S_SLOT_MODE_MONO 1
#define I2S_SLOT_MODE_STEREO 2
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_BIT_WIDTH_AUTO 0
#define I2S_TDM_SLOT0 1
#define I2S_TDM_SLOT1 2
#define I2S_TDM_AUTO_WS_WIDTH 0
#define I2S_LL_SLOT_FRAME_BIT_MAX 512
#define I2S_MCLK_MULTIPLE_384 384
#define I2S_TDM_CLK_DEFAULT_CONFIG(rate) {.sample_rate_hz=(rate)}
#define I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode, mask) \
    {.data_bit_width=(bits), .slot_mode=(mode), .slot_mask=(mask)}
#define AUDIO_RATE 24000
@DMA_DEFINES@
typedef int esp_err_t;
typedef int i2s_tdm_slot_mask_t;
typedef struct { int data_bit_width, slot_bit_width, slot_mode, slot_mask, ws_width;
    bool ws_pol, bit_shift, left_align; unsigned total_slot; } i2s_tdm_slot_config_t;
typedef struct { int sample_rate_hz, mclk_multiple; } i2s_tdm_clk_config_t;
typedef struct { unsigned active_slot, total_slot; struct { unsigned frame_num, buf_size; } dma; } channel_t;
typedef channel_t *i2s_chan_handle_t;
typedef struct { uint8_t bits_per_sample, channel; uint16_t channel_mask; unsigned sample_rate; } esp_codec_dev_sample_info_t;
static unsigned allocated, allocation_calls, clock_rate;
static int i2s_free_dma_desc(i2s_chan_handle_t channel) { channel->dma.buf_size=0; allocated=0; return ESP_OK; }
static int i2s_alloc_dma_desc(i2s_chan_handle_t channel, unsigned bytes) {
    channel->dma.buf_size=bytes; allocated=AUDIO_TX_DESCRIPTORS*bytes; allocation_calls++; return ESP_OK;
}
@SIZE@
@SLOT@
static int i2s_channel_reconfig_tdm_slot(i2s_chan_handle_t channel, const i2s_tdm_slot_config_t *slot) {
    return i2s_tdm_set_slot(channel,slot);
}
static int i2s_channel_reconfig_tdm_clock(i2s_chan_handle_t channel, const i2s_tdm_clk_config_t *clock) {
    (void)channel; clock_rate=clock->sample_rate_hz; return ESP_OK;
}
static int codec_reconfigure(i2s_chan_handle_t channel, esp_codec_dev_sample_info_t *fs) {
    int ret; uint8_t slot_bits=fs->bits_per_sample;
    @RECONFIGURE@
    return ret;
}
#define ES7210_INPUT_MIC1 1
#define ES7210_INPUT_MIC2 2
#define ES7210_INPUT_MIC3 4
#define ES7210_INPUT_MIC4 8
#define ES7210_SEL_MIC1 ES7210_INPUT_MIC1
#define ES7210_SEL_MIC2 ES7210_INPUT_MIC2
#define ES7210_SEL_MIC3 ES7210_INPUT_MIC3
#define ES7210_SEL_MIC4 ES7210_INPUT_MIC4
#define ENABLE_TDM_MAX_NUM 3
#define ES_I2S_NORMAL 0
typedef struct { bool is_open; unsigned mic_select, gain; } audio_codec_es7210_t;
typedef audio_codec_es7210_t audio_codec_if_t;
typedef struct { int ctrl_if; unsigned mic_selected; } es7210_codec_cfg_t;
static unsigned adc_bits, adc_rate;
static int es7210_set_bits(audio_codec_es7210_t *codec, unsigned bits) { (void)codec; adc_bits=bits; return 0; }
static int es7210_config_sample(audio_codec_es7210_t *codec, unsigned rate) { (void)codec; adc_rate=rate; return 0; }
static int es7210_config_fmt(audio_codec_es7210_t *codec, unsigned format) { (void)codec; assert(format==ES_I2S_NORMAL); return 0; }
@ADC_MODE@
@ADC_FORMAT@
static void check_dma(esp_codec_dev_sample_info_t *format) {
    channel_t channel={.dma.frame_num=AUDIO_TX_DESCRIPTOR_FRAMES};
    i2s_tdm_slot_config_t initial=@INITIAL@;
    assert(initial.slot_mask==3 && initial.total_slot==4 && initial.data_bit_width==16);
    assert(i2s_tdm_set_slot(&channel,&initial)==0);
    assert(allocated==5760 && channel.active_slot==2 && channel.total_slot==4);
    assert(channel.dma.buf_size==960 && AUDIO_TX_DESCRIPTORS*AUDIO_TX_DESCRIPTOR_FRAMES*1000/AUDIO_RATE==60);
    assert(codec_reconfigure(&channel,format)==0); // Opening the codec must retain the active mask and wire clock.
    assert(allocated==5760 && allocation_calls==1 && clock_rate==24000 && channel.total_slot==4);
    initial.slot_mask=15; // Reproduce the former allocation without reducing DMA time or changing clocks.
    assert(i2s_tdm_set_slot(&channel,&initial)==0 && allocated==11520 && channel.total_slot==4);
    initial.slot_mask=0;
    assert(i2s_tdm_set_slot(&channel,&initial)==ESP_ERR_INVALID_ARG);
}
static void check_adc(esp_codec_dev_sample_info_t *format) {
    int in_ctrl=1;
    @ADC_CONFIG@
    assert(es7210_cfg.mic_selected==15);
    audio_codec_es7210_t adc={.is_open=true,.mic_select=es7210_cfg.mic_selected,.gain=30};
    assert(es7210_is_tdm_mode(&adc) && es7210_set_fs(&adc,format)==0);
    assert(adc.mic_select==15 && adc.gain==30 && adc_bits==16 && adc_rate==24000);
}
int main(void) {
    @FORMAT@
    assert(AUDIO_CAPTURE_CHANNELS==2 && fs_in.channel_mask==3);
    assert(fs_in.channel==(CONFIG_IDF_TARGET_ESP32S3?2:4));
#if !CONFIG_IDF_TARGET_ESP32S3
    check_dma(&fs_in);
    check_adc(&fs_in);
#endif
}
