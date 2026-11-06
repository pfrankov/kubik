#!/usr/bin/env python3
"""Run actual board formats through the pinned codec and IDF RX sizing path."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ['IDF_PATH'])
CODEC = ROOT / 'firmware/managed_components/espressif__esp_codec_dev'


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


audio = (ROOT / 'firmware/main/audio_board.c').read_text()
tdm = (IDF / 'components/esp_driver_i2s/i2s_tdm.c').read_text()
common = (IDF / 'components/esp_driver_i2s/i2s_common.c').read_text()
data = (CODEC / 'platform/audio_codec_data_i2s.c').read_text()
adc = (CODEC / 'device/es7210/es7210.c').read_text()
initial = block(audio[audio.index('i2s_tdm_config_t tdm_cfg'):], '.slot_cfg =')
initial = initial[initial.index('{'):]
format_in = block(audio, 'esp_codec_dev_sample_info_t fs_in') + ';'
adc_config = block(audio, 'es7210_codec_cfg_t es7210_cfg') + ';'
# Keep the real validation/allocation path; hardware register calls follow it.
slot = block(tdm, 'static esp_err_t i2s_tdm_set_slot(')
slot = slot[:slot.index('    bool is_slave')] + '    return ESP_OK;\n}\n'
size = block(common, 'uint32_t i2s_get_buf_size(')
size = size[:size.index('#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE')] + '    return bufsize;\n}\n'
reconfigure = block(data, 'case I2S_COMM_MODE_TDM:')
reconfigure = reconfigure[reconfigure.index('{') + 1:-1]
adc_mode = block(adc, 'static bool es7210_is_tdm_mode(')
adc_format = block(adc, 'static int es7210_set_fs(')
profile = (ROOT / 'firmware/main/audio_output_tail.h').read_text()
defines = '\n'.join(line for line in profile.splitlines() if line.startswith('#define AUDIO_TX_'))
fixture = (ROOT / 'tools/fixtures/audio_rx_dma_test.c').read_text()
source = fixture.replace('@DMA_DEFINES@', defines).replace('@SIZE@', size).replace('@SLOT@', slot)
source = source.replace('@RECONFIGURE@', reconfigure).replace('@ADC_MODE@', adc_mode).replace('@ADC_FORMAT@', adc_format)
source = source.replace('@INITIAL@', initial).replace('@FORMAT@', format_in).replace('@ADC_CONFIG@', adc_config)

with tempfile.TemporaryDirectory(prefix='kubik-audio-dma-') as temporary:
    path, binary = Path(temporary) / 'test.c', Path(temporary) / 'test'
    path.write_text(source)
    for s3 in (0, 1):
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-fsanitize=address,undefined', f'-DCONFIG_IDF_TARGET_ESP32S3={s3}',
                        '-I', str(ROOT / 'firmware/main'), str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print('audio RX: C6 4-slot clocks/2-slot DMA saves 5760 bytes; all ADCs/gain and S3 channel shape preserved')
