#!/usr/bin/env python3
"""Offline Muse acceptance using the real adapter and upstream software cryptography."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / 'firmware/main'
PAIRING = ROOT / 'firmware/components/muse_pairing'
MANAGED_CJSON = ROOT / 'firmware/managed_components/espressif__cjson/cJSON'


def cjson_path():
    if (MANAGED_CJSON / 'cJSON.h').is_file():
        return MANAGED_CJSON
    idf = os.environ.get('IDF_PATH')
    if idf:
        idf_cjson = Path(idf) / 'components/json/cJSON'
        if (idf_cjson / 'cJSON.h').is_file():
            return idf_cjson
    raise SystemExit('cJSON source required (managed component or IDF_PATH/components/json/cJSON)')


def run(command):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        print(result.stdout + result.stderr)
        raise SystemExit(result.returncode)
    return result.stdout


def adapter(tmp):
    global CJSON
    CJSON = cjson_path()
    (tmp / 'esp_err.h').write_text('''#pragma once
    typedef int esp_err_t;
    #define ESP_OK 0
    #define ESP_FAIL -1
    #define ESP_ERR_NO_MEM -2
    #define ESP_ERR_INVALID_ARG -3
    #define ESP_ERR_INVALID_STATE -4
    #define ESP_ERR_NVS_NOT_FOUND -5
    ''')
    (tmp / 'esp_http_server.h').write_text('''#pragma once
    #include <stdbool.h>
    typedef struct {int content_len;const char *body;unsigned offset;bool fail;} httpd_req_t;
    ''')
    names = ('muse_store.c', 'muse_json.c', 'muse_options.c', 'muse_control.c', 'setup_request.c', 'muse_pair.c')
    source = '\n'.join('\n'.join(line for line in (MAIN / name).read_text().splitlines()
                                if not line.startswith('#include')) for name in names)
    fixture = (ROOT / 'tools/fixtures/muse_native_test.c').read_text()
    source = fixture.replace('/* PRODUCTION */', source)
    source_file, exe = tmp / 'adapter.c', tmp / 'adapter'
    source_file.write_text(source)
    run(['cc', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
         '-Wno-unused-variable', '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-I' + str(tmp), '-I' + str(MAIN),
         '-I' + str(CJSON), source_file, CJSON / 'cJSON.c', '-o', exe])
    print(run([exe]).strip())


def pairing(tmp):
    idf = os.environ.get('IDF_PATH')
    if not idf:
        raise SystemExit('Activate ESP-IDF 5.5.1 before Muse acceptance (IDF_PATH required)')
    crypto = Path(idf) / 'components/mbedtls/mbedtls'
    build = ROOT / 'dist/native-tests/muse-crypto'
    run(['cmake', '-S', crypto, '-B', build, '-DENABLE_PROGRAMS=OFF', '-DENABLE_TESTING=OFF'])
    run(['cmake', '--build', build, '--target', 'mbedcrypto', '--parallel', '4'])
    exe = tmp / 'pairing'
    run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
         '-Wno-deprecated-declarations', '-DTEST_PAIRING_EFUSE_AUTH=0', '-I' + str(PAIRING / 'tests/pairing_fakes'), '-I' + str(PAIRING),
         '-I' + str(CJSON), '-I' + str(crypto / 'include'), PAIRING / 'tests/link_pairing_handshake_harness.c',
         PAIRING / 'pairing_transcript.c', PAIRING / 'pairing_signer_policy.c', CJSON / 'cJSON.c',
         build / 'library/libmbedcrypto.a', '-o', exe])
    print(run([exe]).strip())


def noise(tmp):
    idf = Path(os.environ['IDF_PATH'])
    crypto = idf / 'components/mbedtls/mbedtls'
    core = ROOT / 'firmware/components/noise_core'
    exe = tmp / 'noise'
    run(['c++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-I' + str(core / 'include'), '-I' + str(crypto / 'include'),
         ROOT / 'tools/fixtures/muse_noise_inplace_test.cc', core / 'src/PsaCryptoBackend.cpp',
         core / 'src/Status.cpp', ROOT / 'dist/native-tests/muse-crypto/library/libmbedcrypto.a', '-o', exe])
    print(run([exe]).strip())
    print(run([os.sys.executable, '-m', 'unittest', 'discover', '-s',
               ROOT / 'firmware/components/muse_chat/tests', '-p', 'test_muse_chat_link_errors.py']).strip())


def main():
    global CJSON
    CJSON = cjson_path()
    with tempfile.TemporaryDirectory(prefix='kubik-muse-test-') as directory:
        tmp = Path(directory)
        adapter(tmp)
        pairing(tmp)
        noise(tmp)


if __name__ == '__main__':
    main()
