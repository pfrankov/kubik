#!/usr/bin/env python3
"""Exercise production device-key persistence with real Mbed TLS crypto."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / 'firmware/main'
FIXTURE = ROOT / 'firmware/sim/devkey_test.c'


def run(command, *, cwd=ROOT):
    result = subprocess.run(list(map(str, command)), cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        print(result.stdout + result.stderr)
        raise SystemExit(result.returncode)
    return result.stdout


def write_stubs(directory):
    (directory / 'freertos').mkdir()
    files = {
        'esp_err.h': '''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
''',
        'esp_log.h': '''#pragma once
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
''',
        'esp_random.h': '''#pragma once
#include <stddef.h>
void esp_fill_random(void *output, size_t size);
''',
        'freertos/FreeRTOS.h': '''#pragma once
typedef int BaseType_t;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
''',
        'freertos/semphr.h': '''#pragma once
#include <pthread.h>
#include "FreeRTOS.h"
typedef pthread_mutex_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, unsigned int wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex);
''',
        'nvs.h': '''#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size);
esp_err_t nvs_commit(nvs_handle_t handle);
''',
        'nvs_flash.h': '''#pragma once
#include "esp_err.h"
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_deinit(void);
''',
    }
    for name, content in files.items():
        (directory / name).write_text(content)


def build_mbedcrypto(temp):
    idf = os.environ.get('IDF_PATH')
    if not idf:
        raise SystemExit('Activate ESP-IDF 5.5.1 first (IDF_PATH is required)')
    source = Path(idf) / 'components/mbedtls/mbedtls'
    build = temp / 'mbedtls-build'
    header = source / 'include/mbedtls/build_info.h'
    if not header.is_file() or '#define MBEDTLS_VERSION_STRING         "3.6.4"' not in header.read_text():
        raise SystemExit(f'Expected ESP-IDF Mbed TLS 3.6.4 at {source}')
    run(['cmake', '-S', source, '-B', build,
         '-DENABLE_PROGRAMS=OFF', '-DENABLE_TESTING=OFF'])
    run(['cmake', '--build', build, '--target', 'mbedcrypto', '--parallel', '4'])
    return source, build / 'library/libmbedcrypto.a'


def compile_fixture(temp, source, library):
    include = source / 'include'
    executable = temp / 'devkey-test'
    run(['cc', '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
         '-Wno-deprecated-declarations', '-DAPP_NVS_TEST_LOCK',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
         '-I' + str(temp / 'stubs'), '-I' + str(MAIN), '-I' + str(include),
         '-I' + str(source / '3rdparty/everest/include'),
         '-I' + str(source / '3rdparty/p256-m/include'),
         MAIN / 'app_nvs.c', MAIN / 'devkey.c', FIXTURE, library, '-o', executable])
    return executable


def seed(path, blob):
    path.write_bytes(struct.pack('<I', len(blob)) + blob)


def execute(executable, scenario, path, expected_public=None):
    command = [executable, scenario, path]
    if expected_public:
        command.append(expected_public)
    output = run(command)
    line = output.strip()
    if not line.startswith(f'PASS {scenario}'):
        raise SystemExit(f'Unexpected fixture output: {line}')
    print(line)
    return line


def test_matrix(executable, temp):
    empty_store = temp / 'created.nvs'
    created = execute(executable, 'missing', empty_store)
    marker = ' PUBLIC='
    if marker not in created:
        raise SystemExit('Missing-key fixture did not expose its generated public key')
    public = created.split(marker, 1)[1]
    if len(public) != 88:
        raise SystemExit('Generated public key has an unexpected encoding')

    execute(executable, 'restart', empty_store, public)
    execute(executable, 'open-eio', empty_store)
    execute(executable, 'read-eio', empty_store)

    for name, blob in (
        ('short', bytes(range(31))),
        ('long', bytes(range(33))),
        ('bad-zero', bytes(32)),
        ('bad-order', bytes.fromhex('ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551')),
    ):
        path = temp / f'{name}.nvs'
        seed(path, blob)
        execute(executable, name, path)

    execute(executable, 'mutex-failure', temp / 'mutex.nvs')
    for scenario in ('set-before', 'set-after', 'commit-before', 'commit-after',
                     'recovery-open-eio', 'recovery-read-eio', 'recovery-init-fail'):
        execute(executable, scenario, temp / f'{scenario}.nvs')


def main():
    with tempfile.TemporaryDirectory(prefix='kubik-devkey-test-') as directory:
        temp = Path(directory)
        stubs = temp / 'stubs'
        stubs.mkdir()
        write_stubs(stubs)
        source, library = build_mbedcrypto(temp)
        executable = compile_fixture(temp, source, library)
        test_matrix(executable, temp)
    print('PASS devkey production persistence matrix (real Mbed TLS P-256)')


if __name__ == '__main__':
    main()
