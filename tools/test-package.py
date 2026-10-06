#!/usr/bin/env python3
"""Exercise the actual release images, integrity checks, and flash command."""

import argparse
import importlib.util
import gzip
import json
import math
import os
import runpy
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--character", choices=("TESS", "PLUSH"), default="TESS")
CHARACTER = parser.parse_args().character
BUILD = ROOT / ("firmware/build" if CHARACTER == "TESS" else "firmware/build-plush")
FIRMWARE = ROOT / f"dist/kubik-firmware-{CHARACTER.lower()}.zip"


def run_check(bundle, *extra, env=None):
    return subprocess.run(
        [sys.executable, str(bundle / "flash-device.py"), "--bundle", str(bundle), *extra],
        capture_output=True, text=True, env=env,
    )


def check_size_margin():
    """The app partition must keep 2% free: the packager refuses a firmware that leaves less."""
    spec = importlib.util.spec_from_file_location("package_firmware", ROOT / "tools/package-firmware.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    partition = module.app_partition_size(CHARACTER)
    assert partition == (0x300000 if CHARACTER == 'TESS' else 0x1E0000)
    limit = partition - math.ceil(partition * module.MIN_APP_FREE)
    assert module.check_app_margin(limit, partition) == partition - limit
    try:
        module.check_app_margin(limit + 1, partition)
    except ValueError as error:
        assert "at least 2% must stay free" in str(error)
    else:
        raise AssertionError("a firmware leaving under 2% of the app partition free was accepted")


def check_plush_config():
    spec = importlib.util.spec_from_file_location('plush_config', ROOT / 'tools/configure-plush.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory(prefix='kubik-plush-config-') as temporary:
        directory = Path(temporary)
        source, defaults, output = (directory / name for name in ('shared', 'defaults', 'selected'))
        common = 'CONFIG_BT_ENABLED=y\nCONFIG_CUSTOM_OPTION=17\n'
        original = common + 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"\nCONFIG_PARTITION_TABLE_FILENAME="partitions.csv"\n'
        source.write_text(original); defaults.write_text('CONFIG_WIFI_ENABLED=y\n')
        module.configure(source, defaults, output)
        expected = original.replace('partitions.csv', 'partitions-plush.csv')
        assert output.read_text() == expected and source.read_text() == original
        output.write_text(expected.replace('OPTION=17', 'OPTION=19'))
        module.configure(source, defaults, output)
        assert 'OPTION=19' in output.read_text(), 'Existing build-local menuconfig must survive'
        source.unlink(); output.unlink()
        module.configure(source, defaults, output)
        assert output.read_text().startswith(defaults.read_text())
        assert output.read_text().count('="partitions-plush.csv"') == 2



def check_freshness():
    module = runpy.run_path(str(ROOT / 'tools/package-firmware.py'))
    inputs = module['build_inputs'](BUILD)
    wake = ROOT / 'firmware/main/wake_model.cc'
    assert (wake in inputs) == (CHARACTER == 'TESS')
    assert ((ROOT / 'firmware/main/wake_probability.h') in inputs) == (CHARACTER == 'TESS')
    module['check_build_freshness'](BUILD)
    for path, stale in ((wake, CHARACTER == 'TESS'), (ROOT / 'firmware/main/main.c', True)):
        stat = path.stat()
        try:
            os.utime(path, ns=(stat.st_atime_ns, (BUILD / 'kubik.bin').stat().st_mtime_ns + 1_000_000_000))
            try: module['check_build_freshness'](BUILD)
            except ValueError: assert stale
            else: assert not stale, 'stale selected source accepted'
        finally: os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    print('package freshness: actual C/C++/header dependencies; excluded character changes do not block')

def check_bridge_selection():
    spec = importlib.util.spec_from_file_location("flash_device", ROOT / "tools/flash-device.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    target = "/dev/cu.usbmodem101"
    command = f"node tools/usb-bridge/bridge.mjs --port {target} --server ws://localhost:18999"
    assert module.bridge_uses_port(command, target)
    assert not module.bridge_uses_port(command, "/dev/kubik-mock")
    assert not module.bridge_uses_port(command.replace(target, "/dev/cu.usbmodem102"), target)
    assert not module.bridge_uses_port(f"python script.py '{command}'", target)
    assert not module.bridge_uses_port("node --eval tools/usb-bridge/bridge.mjs --port " + target, target)
    assert not module.bridge_uses_port("node other.mjs 'unterminated", target)
    assert not module.bridge_uses_port("node other.mjs --port " + target, target)


def check_web_assets():
    compress = runpy.run_path(str(ROOT / 'tools/assets/compress-web.py'))['compress']
    image = (BUILD / 'kubik.bin').read_bytes()
    for name in ('portal.html', 'portal.css', 'portal_tess.js'):
        source = (ROOT / 'firmware/main' / name).read_bytes()
        packed = (BUILD / 'esp-idf/main' / (name + '.gz')).read_bytes()
        assert gzip.decompress(packed) == source
        assert packed == compress(source) and packed[4:8] == bytes(4)
        assert image.count(packed) == 1 and source not in image
    print('web assets: exact reproducible gzip, one embedded copy, no plain duplicate')


def check_build_choice():
    cmake = (ROOT / 'firmware/main/CMakeLists.txt').read_text()
    choice = cmake[cmake.index('set(KUBIK_CHARACTER'):cmake.index('set(WAKE_SRCS')]
    for character in ('TESS', 'PLUSH'):
        with tempfile.TemporaryDirectory(prefix='kubik-choice-') as tmp:
            directory = Path(tmp)
            script = directory / 'choice.cmake'
            script.write_text(choice)
            def configure(value):
                return subprocess.run(['cmake', f'-DKUBIK_CHARACTER={value}', '-P', str(script)],
                                      cwd=directory, text=True, capture_output=True)
            assert configure(character).returncode == 0
            assert configure(character).returncode == 0
            other = 'PLUSH' if character == 'TESS' else 'TESS'
            assert 'separate build directory' in configure(other).stderr
            assert 'must be TESS or PLUSH' in configure('BOTH').stderr
            assert (directory / 'character.txt').read_text().strip() == character


def check_character_symbols():
    cache = (BUILD / 'CMakeCache.txt').read_text()
    nm = re.search(r'^CMAKE_NM:FILEPATH=(.+)$', cache, re.M).group(1)
    symbols = {line.split()[0] for line in subprocess.check_output(
        [nm, '--defined-only', '--format=posix', str(BUILD / 'kubik.elf')], text=True).splitlines() if line.strip()}
    tess = {'tess_draw', 'tess_update', 'tess_sound_mix', 'tess_reset', 'app_wake_init', 'app_wake_listening'}
    plush = {'face_plush_draw', 'body_update', 'assets_pcm'}
    required, absent = (tess, plush) if CHARACTER == 'TESS' else (plush, tess)
    # Both builds end native voice input by local VAD; wake-word inference is Tess-only.
    assert {'assets_init', 'vad_create_with_param', 'app_voice_start_capture'} <= symbols
    if CHARACTER == 'TESS': assert 'assets_data' in symbols
    else: assert not any('tflite' in name.lower() or name.startswith('wake_model') for name in symbols)
    assert required <= symbols, required - symbols
    assert not absent & symbols, absent & symbols
    print(f'{CHARACTER}: only the selected character renderer, motion and sound are linked')


def check_manifest_character(path, manifest):
    manifest_path = path / 'manifest.json'
    for value in (None, '', 'Both', 'tess'):
        invalid = dict(manifest)
        if value is None: invalid.pop('character')
        else: invalid['character'] = value
        manifest_path.write_text(json.dumps(invalid))
        result = run_check(path, '--check')
        assert result.returncode != 0 and 'character' in result.stderr
    manifest_path.write_text(json.dumps(manifest))


def check_sound_inventory():
    directory = ROOT / 'firmware/assets/sfx'
    entries = json.loads((directory / 'manifest.json').read_text())
    names = [entry['file'] for entry in entries]
    assert len(names) == len(set(names))
    assert set(names) == {path.name for path in directory.glob('*.pcm')}
    for entry in entries:
        assert (directory / entry['file']).stat().st_size == entry['ms'] * 48


def main():
    check_size_margin()
    check_plush_config()
    check_bridge_selection()
    check_web_assets()
    check_character_symbols()
    check_freshness()
    check_build_choice()
    check_sound_inventory()
    subprocess.run([sys.executable, str(ROOT / "tools/package-firmware.py"), "--character", CHARACTER], check=True)
    with tempfile.TemporaryDirectory(prefix="kubik-bundle-test-") as temporary:
        path = Path(temporary)
        with zipfile.ZipFile(FIRMWARE) as release:
            release.extractall(path)
        manifest = json.loads((path / "manifest.json").read_text())
        assert manifest["character"] == CHARACTER.title()
        check_manifest_character(path, manifest)
        assets = (path / "assets.bin").read_bytes()
        if CHARACTER == "TESS":
            assert assets[:8] == b"KAST" + (1).to_bytes(4, "little")
            assert assets[8:32].rstrip(b"\0") == b"wake/tessa"
            assert assets[40:] == (ROOT / "firmware/assets/wake/tessa.tflite").read_bytes()
        else:
            assert len(assets) > 10_000_000 and b"wake/tessa" not in assets[:8192]
        assert (path / "licenses/OFL-PT-Sans.txt").is_file()
        assert run_check(path, "--check").returncode == 0

        module = path / "esptool"
        module.mkdir()
        (module / "__init__.py").write_text("")
        (module / "__main__.py").write_text(
            "import json,os,sys\n"
            "open(os.environ['KUBIK_FLASH_ARGS'],'w').write(json.dumps(sys.argv[1:]))\n"
        )
        args_file = path / "flash-args.json"
        env = {**os.environ, "PYTHONPATH": str(path), "KUBIK_FLASH_ARGS": str(args_file)}
        flashed = run_check(path, "--port", "/dev/kubik-mock", env=env)
        assert flashed.returncode == 0, (flashed.stdout, flashed.stderr)
        assert "Запись завершена" in flashed.stdout and "PWR" in flashed.stdout
        args = json.loads(args_file.read_text())
        assert args[:args.index("write_flash")].count("/dev/kubik-mock") == 1
        for part in manifest["parts"]:
            expected = [hex(part["offset"]), str(path.resolve() / part["file"])]
            assert expected in [args[i:i + 2] for i in range(len(args) - 1)]

        (module / "__main__.py").write_text(
            "import sys\n"
            "print('Download mode detected; no sync reply', file=sys.stderr)\n"
            "raise SystemExit(2)\n"
        )
        no_sync = run_check(path, "--port", "/dev/kubik-mock", env=env)
        assert no_sync.returncode == 1
        assert "отключите USB-C" in no_sync.stderr and "PWR" in no_sync.stderr
        assert "Command '['" not in no_sync.stderr

        image = path / manifest["parts"][2]["file"]
        data = bytearray(image.read_bytes())
        data[-1] ^= 1
        image.write_bytes(data)
        failed = run_check(path, "--check")
        assert failed.returncode != 0 and "integrity check" in failed.stderr
        assert run_check(path, "--port", "/dev/kubik-mock", env=env).returncode != 0
    print("release: 2% app partition margin enforced, real images verified, mock flash layout exact, tampering rejected before write")


if __name__ == "__main__":
    main()
