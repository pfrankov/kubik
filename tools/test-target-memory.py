#!/usr/bin/env python3
"""Run the wake FFT allocator for both chips; constrained pools cannot spill into DMA RAM."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
wake = (ROOT / 'firmware/main/wake_model.cc').read_text()
profile = wake[wake.index('#if CONFIG_IDF_TARGET_ESP32S3'):wake.index('// Used only by the SDK')]
allocator = wake[wake.index('extern "C" void *kubik_wake_fft_alloc'):wake.index('static tflite::MicroMutableOpResolver')]
fixture = r'''
#include <cassert>
#include <cstddef>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_RTCRAM 2
#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_INTERNAL 8
static unsigned calls, observed_caps;
static size_t available, observed_bytes;
static char slot;
static void *heap_caps_malloc(size_t bytes, unsigned caps) {
    ++calls; observed_caps=caps; observed_bytes=bytes;
    return bytes<=available ? &slot : nullptr;
}
'''
checks = r'''
int main() {
    constexpr unsigned expected = (CONFIG_IDF_TARGET_ESP32S3 ? MALLOC_CAP_SPIRAM : MALLOC_CAP_RTCRAM)|MALLOC_CAP_8BIT;
    assert(TENSOR_CAPS == ((CONFIG_IDF_TARGET_ESP32S3 ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL)|MALLOC_CAP_8BIT));
    assert(SCRATCH_BYTES >= (CONFIG_IDF_TARGET_ESP32S3 ? 11824u : 6536u));
    assert(SCRATCH_BYTES <= (CONFIG_IDF_TARGET_ESP32S3 ? 16384u : 8192u));
    assert(PERSISTENT_BYTES >= 16436 && VARIABLES_BYTES == 1024);
    available=4096;
    assert(kubik_wake_fft_alloc(4096)==&slot && observed_caps==expected && observed_bytes==4096 && calls==1);
    assert(kubik_wake_fft_alloc(4097)==nullptr && observed_caps==expected && observed_bytes==4097 && calls==2);
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-target-memory-') as temporary:
    directory = Path(temporary)
    source, binary = directory / 'allocator.cc', directory / 'allocator'
    source.write_text(fixture + profile + allocator + checks)
    for s3 in (0, 1):
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        f'-DCONFIG_IDF_TARGET_ESP32S3={s3}', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)
print('wake FFT: C6 RTC / S3 PSRAM allocation and exact refusal boundary; no internal-memory spill')
