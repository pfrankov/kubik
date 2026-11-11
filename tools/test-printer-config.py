#!/usr/bin/env python3
"""A warm board config must enable printer discovery without losing user menuconfig."""
from pathlib import Path
import importlib.util
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('printer_config', ROOT / 'tools/configure-printer.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
def verify_target(base, target):
    defaults, output = base / 'defaults', base / 'sdkconfig'
    defaults.write_text('CONFIG_BT_NIMBLE_ROLE_CENTRAL=y\nCONFIG_BT_NIMBLE_SECURITY_ENABLE=n\n')
    value = 'n' if target == 'esp32c6' else 'y'
    allocation = 'CONFIG_BT_NIMBLE_MSYS_BUF_FROM_HEAP'
    canonical = f'{allocation}=y' if value == 'y' else f'# {allocation} is not set'
    profile = defaults.with_name(defaults.name + '.' + target)
    profile.write_text(f'{allocation}={value}\n')
    assert module.configure(output, defaults, target) is False and not output.exists()
    for old in ('# CONFIG_BT_NIMBLE_ROLE_CENTRAL is not set\n',
                'CONFIG_BT_NIMBLE_ROLE_CENTRAL=n\n', '',
                'CONFIG_BT_NIMBLE_ROLE_CENTRAL=n\nCONFIG_BT_NIMBLE_ROLE_CENTRAL=y\n'):
        output.write_text('CUSTOM_AUDIO=19\nCONFIG_PARTITION_TABLE_CUSTOM_FILENAME="custom.csv"\n' + old)
        assert module.configure(output, defaults, target)
        text = output.read_text()
        assert text.count('CONFIG_BT_NIMBLE_ROLE_CENTRAL=y') == 1
        assert text.count('# CONFIG_BT_NIMBLE_SECURITY_ENABLE is not set') == 1
        assert 'CUSTOM_AUDIO=19\n' in text and '"custom.csv"' in text
        assert not module.configure(output, defaults, target)
    valid = 'CUSTOM_AUDIO=20\nCONFIG_BT_NIMBLE_ROLE_CENTRAL=y\n# CONFIG_BT_NIMBLE_SECURITY_ENABLE is not set\nOTHER=on\n'
    if target == 'esp32c6':
        valid += canonical + '\n'
    output.write_text(valid)
    stamp = output.stat().st_mtime_ns
    assert not module.configure(output, defaults, target)
    assert output.read_text() == valid and output.stat().st_mtime_ns == stamp
    for previous in ('CONFIG_BT_NIMBLE_SECURITY_ENABLE=y\n', 'CONFIG_BT_NIMBLE_SECURITY_ENABLE=n\n',
                     'CONFIG_BT_NIMBLE_SECURITY_ENABLE=y\nCONFIG_BT_NIMBLE_SECURITY_ENABLE=n\n'):
        output.write_text('CUSTOM_AUDIO=22\nCONFIG_BT_NIMBLE_ROLE_CENTRAL=y\n' + previous)
        assert module.configure(output, defaults, target)
        assert output.read_text().count('# CONFIG_BT_NIMBLE_SECURITY_ENABLE is not set') == 1
        assert 'CUSTOM_AUDIO=22\n' in output.read_text()
        assert not module.configure(output, defaults, target)
    for previous in ('', f'{allocation}=y\n', f'{allocation}=n\n',
                     f'{allocation}=y\n{allocation}=n\n'):
        sample = valid.replace(canonical + '\n', previous) if target == 'esp32c6' else valid + previous
        output.write_text(sample)
        module.configure(output, defaults, target)
        if target == 'esp32c6':
            assert output.read_text() == valid
        else:
            assert output.read_text() == sample, 'S3 allocator policy must remain untouched'
        assert not module.configure(output, defaults, target)
    invalid_shared(output, defaults, target)

def invalid_shared(output, defaults, target):
    output.write_text('CUSTOM_AUDIO=21\n')
    for bad in ('CONFIG_BT_NIMBLE_ROLE_CENTRAL=n\n', '',
                'CONFIG_BT_NIMBLE_ROLE_CENTRAL=y\nCONFIG_BT_NIMBLE_ROLE_CENTRAL=y\n',
                'CONFIG_BT_NIMBLE_ROLE_CENTRAL=y\nCONFIG_BT_NIMBLE_SECURITY_ENABLE=y\n',
                'CONFIG_BT_NIMBLE_ROLE_CENTRAL=y\nCONFIG_BT_NIMBLE_SECURITY_ENABLE=n\nCONFIG_BT_NIMBLE_SECURITY_ENABLE=n\n'):
        defaults.write_text(bad)
        try:
            module.configure(output, defaults, target)
            raise AssertionError('Invalid authoritative defaults were accepted')
        except ValueError:
            assert output.read_text() == 'CUSTOM_AUDIO=21\n'

with tempfile.TemporaryDirectory(prefix='kubik-printer-config-') as directory:
    for target in ('esp32c6', 'esp32s3'):
        base = Path(directory) / target
        base.mkdir()
        verify_target(base, target)
        defaults, output = base / 'defaults', base / 'sdkconfig'
        defaults.write_text('CONFIG_BT_NIMBLE_ROLE_CENTRAL=y\nCONFIG_BT_NIMBLE_SECURITY_ENABLE=n\n')
        profile = defaults.with_name(defaults.name + '.' + target)
        if target != 'esp32c6':
            continue
        good = 'n'
        for bad in ('', f'CONFIG_BT_NIMBLE_MSYS_BUF_FROM_HEAP={"y" if good == "n" else "n"}\n',
                    f'CONFIG_BT_NIMBLE_MSYS_BUF_FROM_HEAP={good}\n' * 2):
            profile.write_text(bad)
            output.write_text('CUSTOM_AUDIO=21\n')
            try:
                module.configure(output, defaults, target)
                raise AssertionError('Invalid target allocation policy was accepted')
            except ValueError:
                assert output.read_text() == 'CUSTOM_AUDIO=21\n'
cmake = (ROOT / 'firmware/CMakeLists.txt').read_text()
assert cmake.index('tools/configure-printer.py') < cmake.index('include($ENV{IDF_PATH}/tools/cmake/project.cmake)')
assert 'tools/configure-printer.py' in (ROOT / 'tools/package-firmware.py').read_text()
assert '"${IDF_TARGET}"' in cmake[cmake.index('tools/configure-printer.py'):cmake.index('include($ENV{IDF_PATH}/tools/cmake/project.cmake)')]
print('Printer config: both target policies, fresh/warm/missing/duplicate, idempotence, invalid authorities and preserved options passed')
