#!/usr/bin/env python3
"""Exercise the bundled installer CLI with a mock esptool; never open USB."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch

from firmware_layout import TARGETS

ROOT = Path(__file__).resolve().parent.parent
PORT = '/dev/kubik-mock'
MOCK = '''import json, os, pathlib, sys
record = pathlib.Path(os.environ['KUBIK_RESET_CALLS'])
calls = record.read_text().splitlines() if record.exists() else []
with record.open('a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\\n')
if int(os.environ.get('KUBIK_RESET_FAIL_AT', '0')) == len(calls) + 1:
    raise SystemExit(7)
'''


def make_bundle(directory, target):
    directory.mkdir()
    for name in ('flash-device.py', 'firmware_layout.py'):
        shutil.copy2(ROOT / 'tools' / name, directory / name)
    module = directory / 'esptool'
    module.mkdir()
    (module / '__init__.py').write_text('')
    (module / '__main__.py').write_text(MOCK)
    manifest = {'chip': target, 'board': TARGETS[target]['board'], 'flash_size': '16MB',
                'character': 'Tess', 'version': 'reset-test', 'parts': []}
    for offset, name in ((0, 'bootloader.bin'), (0x8000, 'partition-table.bin'),
                         (0x20000, 'kubik.bin'), (0x320000, 'assets.bin')):
        data = bytearray(32)
        if name in {'bootloader.bin', 'kubik.bin'}:
            data[0] = 0xe9
            data[12:14] = TARGETS[target]['chip_id'].to_bytes(2, 'little')
        else:
            data[:] = b'fixture image'
        (directory / name).write_bytes(data)
        manifest['parts'].append({'offset': offset, 'file': name, 'size': len(data),
                                  'sha256': hashlib.sha256(data).hexdigest()})
    (directory / 'manifest.json').write_text(json.dumps(manifest))
    return manifest


def run_bundle(directory, fail_at=0, check=False):
    record = directory / 'calls.jsonl'
    record.unlink(missing_ok=True)
    env = {**os.environ, 'PYTHONPATH': str(directory), 'KUBIK_RESET_CALLS': str(record),
           'KUBIK_RESET_FAIL_AT': str(fail_at)}
    command = [sys.executable, str(directory / 'flash-device.py'), '--port', PORT]
    if check:
        command.append('--check')
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
    calls = [json.loads(line) for line in record.read_text().splitlines()] if record.exists() else []
    return result, calls


def expected_commands(directory, manifest):
    target = manifest['chip']
    s3 = target == 'esp32s3'
    common = ['--chip', target, '--port', PORT]
    flash = common + ['--baud', '460800', '--before', 'usb_reset' if s3 else 'default_reset',
                      '--after', 'no_reset' if s3 else 'hard_reset', 'write_flash',
                      '--flash_mode', 'dio', '--flash_size', '16MB', '--flash_freq', '80m']
    for part in manifest['parts']:
        flash += [hex(part['offset']), str(directory / part['file'])]
    commands = [flash]
    if s3:
        commands += [common + ['--before', 'no_reset', '--after', 'watchdog_reset',
                               'write_mem', '0x6000812c', '0', '0x1']]
    return commands


def check_reset_timeout(directory, manifest):
    spec = importlib.util.spec_from_file_location('installer', directory / 'flash-device.py')
    installer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(installer)
    calls = []

    def run(command, check, timeout=None):
        assert check
        calls.append(command[3:])
        if 'write_mem' in command:
            assert timeout == 30
            raise subprocess.TimeoutExpired(command, timeout)

    with patch.object(installer, 'stop_bridges'), patch.object(installer.subprocess, 'run', side_effect=run):
        try:
            installer.flash_images(directory, manifest, PORT)
        except RuntimeError as error:
            assert 'перезапуск ESP32-S3 не завершён' in str(error)
        else:
            raise AssertionError('Timed-out reset was reported as successful')
    assert calls == expected_commands(directory, manifest)


with tempfile.TemporaryDirectory(prefix='kubik-flash-reset-') as temporary:
    for target in TARGETS:
        directory = (Path(temporary) / target).resolve()
        manifest = make_bundle(directory, target)
        expected = expected_commands(directory, manifest)
        checked, calls = run_bundle(directory, check=True)
        assert checked.returncode == 0 and not calls
        result, calls = run_bundle(directory)
        assert result.returncode == 0 and calls == expected, (result.stdout, result.stderr, calls)
        assert 'Запись завершена' in result.stdout
        if target == 'esp32s3':
            check_reset_timeout(directory, manifest)
        for failure in range(1, len(expected) + 1):
            failed, calls = run_bundle(directory, fail_at=failure)
            assert failed.returncode == 1 and calls == expected[:failure]
            assert 'Запись завершена' not in failed.stdout
            assert 'Traceback' not in failed.stderr
            if failure == 2:
                assert 'перезапуск ESP32-S3 не завершён' in failed.stderr
        # A checksum failure rejects the bundle before either flash or reset.
        image = directory / 'kubik.bin'
        image.write_bytes(image.read_bytes() + b'corrupt')
        failed, calls = run_bundle(directory)
        assert failed.returncode == 1 and not calls and 'integrity check' in failed.stderr
print('flash reset: exact ordered S3 write+RTC/WDT reset, single C6 hard reset, '
      'stage failure propagation, check-only and corruption rejection passed; no USB accessed')
