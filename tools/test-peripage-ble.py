#!/usr/bin/env python3
"""Exercise actual PeriPage transport/profile and shared BLE ownership code."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def startup_test(directory):
    startup = (ROOT / 'firmware/main/main.c').read_text()
    header = (ROOT / 'firmware/main/muse_store.h').read_text()
    state = re.search(r'typedef enum \{[^}]+\} muse_state_t;', header)
    helper = re.search(r'static void release_paired_muse_bluetooth\(muse_state_t state\) \{\n.*?\n\}', startup, re.S)
    assert state and helper
    assert startup.count('esp_bt_mem_release(') == 1
    assert startup.count('release_paired_muse_bluetooth(muse);') == 1
    (directory / 'actual_startup.inc').write_text(state.group() + '\n' + helper.group())
    executable = directory / 'ble_startup'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(directory), str(ROOT / 'tools/fixtures/ble_startup_test.c'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=20)


def source(name):
    return '\n'.join(line for line in (ROOT / 'firmware/main' / name).read_text().splitlines()
                     if not line.startswith(('#include', '#pragma once')))


with tempfile.TemporaryDirectory(prefix='kubik-peripage-') as temporary:
    directory = Path(temporary)
    startup_test(directory)
    common = '\n'.join(source(name) for name in ('ble_runtime.h', 'peripage_ble.h', 'peripage_profile.h', 'peripage_gatt.h'))
    for lane, names in (
        ('peripage_ble', ('peripage_profile.c', 'peripage_gatt.c', 'peripage_ble.c')),
        ('ble_runtime', ('ble_runtime.c',)),
    ):
        body = common + '\n' + '\n'.join(source(name) for name in names)
        (directory / 'actual.inc').write_text(body)
        fixture = ROOT / f'tools/fixtures/{lane}_test.c'
        handles = (0, 42) if lane == 'peripage_ble' else ('c6', 's3')
        for handle in handles:
            executable = directory / f'{lane}-{handle}'
            flags = [f'-DMOCK_HANDLE={handle}'] if lane == 'peripage_ble' else [f'-DCONFIG_IDF_TARGET_ESP32S3={int(handle == "s3")}']
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', *flags,
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-I', str(directory), str(fixture), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=20)

muse = (ROOT / 'firmware/main/muse_ble.c').read_text()
assert 'ble_runtime_start(BLE_RUNTIME_MUSE, prepare_services, synced)' in muse
assert 'nimble_port_init(' not in muse and 'nimble_port_freertos_init(' not in muse
printer = (ROOT / 'firmware/main/peripage_ble.c').read_text()
assert 'ble_gap_terminate(' not in printer  # IDF stop must own its disconnect and wait.
defaults = (ROOT / 'firmware/sdkconfig.defaults').read_text()
assert 'CONFIG_BT_NIMBLE_ROLE_CENTRAL=y' in defaults
assert 'CONFIG_BT_NIMBLE_ROLE_OBSERVER=n' in defaults

print('PeriPage: bounded unique discovery, profile/MTU validation, packet ownership, failure cleanup and BLE runtime lifecycle passed')
