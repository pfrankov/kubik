#!/usr/bin/env python3
"""A warm board config must enable printer discovery without losing user menuconfig."""
from pathlib import Path
import importlib.util
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('printer_config', ROOT / 'tools/configure-printer.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix='kubik-printer-config-') as directory:
    base = Path(directory)
    defaults, output = base / 'defaults', base / 'sdkconfig'
    defaults.write_text('CONFIG_BT_NIMBLE_ROLE_CENTRAL=y\nCONFIG_BT_NIMBLE_SECURITY_ENABLE=n\n')
    assert module.configure(output, defaults) is False and not output.exists()
    for old in ('# CONFIG_BT_NIMBLE_ROLE_CENTRAL is not set\n',
                'CONFIG_BT_NIMBLE_ROLE_CENTRAL=n\n', '',
                'CONFIG_BT_NIMBLE_ROLE_CENTRAL=n\nCONFIG_BT_NIMBLE_ROLE_CENTRAL=y\n'):
        output.write_text('CUSTOM_AUDIO=19\nCONFIG_PARTITION_TABLE_CUSTOM_FILENAME="custom.csv"\n' + old)
        assert module.configure(output, defaults)
        text = output.read_text()
        assert text.count('CONFIG_BT_NIMBLE_ROLE_CENTRAL=y') == 1
        assert text.count('# CONFIG_BT_NIMBLE_SECURITY_ENABLE is not set') == 1
        assert 'CUSTOM_AUDIO=19\n' in text and '"custom.csv"' in text
        assert not module.configure(output, defaults)
    valid = 'CUSTOM_AUDIO=20\nCONFIG_BT_NIMBLE_ROLE_CENTRAL=y\n# CONFIG_BT_NIMBLE_SECURITY_ENABLE is not set\nOTHER=on\n'
    output.write_text(valid)
    stamp = output.stat().st_mtime_ns
    assert not module.configure(output, defaults)
    assert output.read_text() == valid and output.stat().st_mtime_ns == stamp
    for previous in ('CONFIG_BT_NIMBLE_SECURITY_ENABLE=y\n', 'CONFIG_BT_NIMBLE_SECURITY_ENABLE=n\n',
                     'CONFIG_BT_NIMBLE_SECURITY_ENABLE=y\nCONFIG_BT_NIMBLE_SECURITY_ENABLE=n\n'):
        output.write_text('CUSTOM_AUDIO=22\nCONFIG_BT_NIMBLE_ROLE_CENTRAL=y\n' + previous)
        assert module.configure(output, defaults)
        assert output.read_text().count('# CONFIG_BT_NIMBLE_SECURITY_ENABLE is not set') == 1
        assert 'CUSTOM_AUDIO=22\n' in output.read_text()
        assert not module.configure(output, defaults)
    output.write_text('CUSTOM_AUDIO=21\n')
    for bad in ('CONFIG_BT_NIMBLE_ROLE_CENTRAL=n\n', '',
                'CONFIG_BT_NIMBLE_ROLE_CENTRAL=y\nCONFIG_BT_NIMBLE_ROLE_CENTRAL=y\n',
                'CONFIG_BT_NIMBLE_ROLE_CENTRAL=y\nCONFIG_BT_NIMBLE_SECURITY_ENABLE=y\n',
                'CONFIG_BT_NIMBLE_ROLE_CENTRAL=y\nCONFIG_BT_NIMBLE_SECURITY_ENABLE=n\nCONFIG_BT_NIMBLE_SECURITY_ENABLE=n\n'):
        defaults.write_text(bad)
        try:
            module.configure(output, defaults)
            raise AssertionError('Invalid authoritative defaults were accepted')
        except ValueError:
            assert output.read_text() == 'CUSTOM_AUDIO=21\n'
cmake = (ROOT / 'firmware/CMakeLists.txt').read_text()
assert cmake.index('tools/configure-printer.py') < cmake.index('include($ENV{IDF_PATH}/tools/cmake/project.cmake)')
assert 'tools/configure-printer.py' in (ROOT / 'tools/package-firmware.py').read_text()
print('Printer config: fresh/default, warm disabled/missing/duplicate, idempotence and preserved options passed')
