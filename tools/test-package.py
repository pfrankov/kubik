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
import shlex
import shutil
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



def check_compiler_inputs():
    module = runpy.run_path(str(ROOT / 'tools/package-firmware.py'))
    cache = (BUILD / 'CMakeCache.txt').read_text()
    ninja = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$', cache, re.M).group(1)
    cc = shutil.which('cc')
    cxx = shutil.which('c++')
    assert cc and cxx, 'C and C++ compilers are required for the Ninja dependency fixture'
    with tempfile.TemporaryDirectory(prefix='kubik-ninja-deps-') as temporary:
        build = Path(temporary)
        source = build / 'src'
        include = build / 'include'
        source.mkdir()
        include.mkdir()
        (include / 'shared header.h').write_text('static inline int shared_value(void) { return 41; }\n')
        (include / 'excluded header.h').write_text('static inline int excluded_value(void) { return 7; }\n')
        (source / 'selected.c').write_text('#include "shared header.h"\nint c_value(void) { return shared_value(); }\n')
        (source / 'selected.cpp').write_text('#include "shared header.h"\nint cpp_value() { return shared_value(); }\n')
        (source / 'excluded.c').write_text('#include "excluded header.h"\nint excluded(void) { return excluded_value(); }\n')
        (build / 'CMakeCache.txt').write_text(f'CMAKE_MAKE_PROGRAM:FILEPATH={ninja}\n')
        (build / 'build.ninja').write_text(
            'rule cc\n'
            f'  command = {shlex.quote(cc)} -MMD -MF $out.d -MT $out -I include -c $in -o $out\n'
            '  depfile = $out.d\n'
            '  deps = gcc\n'
            'rule cxx\n'
            f'  command = {shlex.quote(cxx)} -MMD -MF $out.d -MT $out -I include -c $in -o $out\n'
            '  depfile = $out.d\n'
            '  deps = gcc\n'
            'build selected-c.o: cc src/selected.c\n'
            'build selected-cpp.o: cxx src/selected.cpp\n'
            'build excluded.o: cc src/excluded.c\n'
        )
        subprocess.run([ninja, '-C', str(build), 'selected-c.o', 'selected-cpp.o', 'excluded.o'], check=True,
                       capture_output=True, text=True)
        selected_relative = [
            {'directory': str(build), 'file': str((source / 'selected.c').resolve()), 'output': 'selected-c.o'},
            {'directory': str(build), 'file': str((source / 'selected.cpp').resolve()), 'output': 'selected-cpp.o'},
        ]
        selected = [dict(entry) for entry in selected_relative]
        selected[1]['output'] = str((build / selected[1]['output']).resolve())
        expected = {
            (source / 'selected.c').resolve(),
            (source / 'selected.cpp').resolve(),
            (include / 'shared header.h').resolve(),
        }
        relative_actual = module['compiler_inputs'](build, selected_relative)
        mixed_actual = module['compiler_inputs'](build, selected)
        assert relative_actual == mixed_actual == expected, (relative_actual, mixed_actual, expected)
        assert (source / 'excluded.c').resolve() not in mixed_actual
        assert (include / 'excluded header.h').resolve() not in mixed_actual

        duplicate_alias = [dict(selected_relative[0]), dict(selected_relative[1])]
        duplicate_alias[1]['output'] = str((build / duplicate_alias[0]['output']).resolve())
        for invalid, label in (
            (duplicate_alias, 'absolute and relative aliases for one Ninja target were accepted'),
            ([dict(selected_relative[0], output=str(build.parent / 'outside.o'))],
             'an output outside the build directory was accepted'),
        ):
            try:
                module['compiler_inputs'](build, invalid)
            except ValueError:
                pass
            else:
                raise AssertionError(label)

        deps_log = build / '.ninja_deps'
        assert deps_log.is_file() and deps_log.stat().st_size > 0
        record = subprocess.run([ninja, '-C', str(build), '-t', 'deps', selected[0]['output']],
                                check=True, capture_output=True, text=True).stdout
        deps_mtime = re.search(r'deps mtime (\d+) \(VALID\)', record)
        assert deps_mtime, record
        output = build / selected[0]['output']
        output_stat = output.stat()
        try:
            stale_mtime = max(output_stat.st_mtime_ns, int(deps_mtime[1])) + 1_000_000_000
            os.utime(output, ns=(output_stat.st_atime_ns, stale_mtime))
            try:
                module['compiler_inputs'](build, selected)
            except ValueError:
                pass
            else:
                raise AssertionError('stale Ninja compiler dependencies were accepted')
        finally:
            os.utime(output, ns=(output_stat.st_atime_ns, output_stat.st_mtime_ns))

        deps_log.unlink()

        def rejects_invalid_log(message):
            try:
                module['compiler_inputs'](build, selected)
            except (OSError, ValueError, subprocess.CalledProcessError):
                return
            raise AssertionError(message)

        rejects_invalid_log('missing .ninja_deps was accepted')
        deps_log.write_bytes(b'not a Ninja dependency log')
        rejects_invalid_log('corrupt .ninja_deps was accepted')

        fake_ninja = build / 'failing-ninja'
        fake_ninja.write_text(
            '#!/usr/bin/env python3\n'
            'import sys\n'
            "sys.stderr.write('DIAGNOSTIC_START' + 'x' * 3000 + 'DIAGNOSTIC_END')\n"
            'sys.exit(2)\n'
        )
        fake_ninja.chmod(0o755)
        (build / 'CMakeCache.txt').write_text(f'CMAKE_MAKE_PROGRAM:FILEPATH={fake_ninja}\n')
        try:
            module['compiler_inputs'](build, selected)
        except ValueError as error:
            assert 'DIAGNOSTIC_START' in str(error)
            assert 'DIAGNOSTIC_END' not in str(error)
            assert len(str(error)) < 1400, 'Ninja failure diagnostics were not bounded'
        else:
            raise AssertionError('a failed Ninja dependency query was accepted')

    with tempfile.TemporaryDirectory(prefix='kubik-compile-commands-') as temporary:
        build = Path(temporary)
        description_path = BUILD / 'project_description.json'
        commands_path = BUILD / 'compile_commands.json'
        shutil.copyfile(description_path, build / description_path.name)
        commands = json.loads(commands_path.read_text())
        missing_source = (ROOT / 'firmware/main/main.c').resolve()
        assert any((Path(entry['directory']) / entry['file']).resolve() == missing_source for entry in commands)
        (build / commands_path.name).write_text(json.dumps([
            entry for entry in commands
            if (Path(entry['directory']) / entry['file']).resolve() != missing_source
        ]))
        try:
            module['build_inputs'](build)
        except ValueError as error:
            assert 'Missing firmware compile commands' in str(error), str(error)
        else:
            raise AssertionError('missing compile command for an owned source was accepted')
    print('package freshness: mixed relative/absolute Ninja outputs normalized; duplicate aliases, outside '
          'outputs, stale/missing/corrupt records, failed-query diagnostics and incomplete compile databases checked')


def check_freshness():
    module = runpy.run_path(str(ROOT / 'tools/package-firmware.py'))
    inputs = module['build_inputs'](BUILD)
    wake = ROOT / 'firmware/main/wake_model.cc'
    wake_header = ROOT / 'firmware/main/wake_probability.h'
    selected_sources = (
        ROOT / 'firmware/main/main.c',
        ROOT / 'firmware/main/muse_noise.cc',
        ROOT / 'firmware/main/muse_chat_helpers.c',
        ROOT / 'firmware/components/muse_pairing/link_pairing.c',
    )
    assert (wake in inputs) == (CHARACTER == 'TESS')
    assert (wake_header in inputs) == (CHARACTER == 'TESS')
    assert all(path in inputs for path in selected_sources), [path for path in selected_sources if path not in inputs]
    module['check_build_freshness'](BUILD)

    checks = [(wake, CHARACTER == 'TESS'), (wake_header, CHARACTER == 'TESS')]
    checks.extend((path, True) for path in selected_sources)
    with tempfile.TemporaryDirectory(prefix='kubik-freshness-') as temporary:
        directory = Path(temporary)
        image = directory / 'kubik.bin'
        baseline = directory / 'baseline.c'
        image.write_bytes(b'build image')
        baseline.write_bytes(b'baseline source')
        image_mtime = image.stat().st_mtime_ns
        baseline_stat = baseline.stat()
        os.utime(baseline, ns=(baseline_stat.st_atime_ns, image_mtime))

        for index, (path, stale) in enumerate(checks):
            candidate = directory / f'candidate-{index}.c'
            candidate.write_bytes(b'candidate source')
            candidate_stat = candidate.stat()
            os.utime(candidate, ns=(candidate_stat.st_atime_ns, image_mtime + 1_000_000_000))
            selected = path in inputs
            assert selected == stale, f'incorrect selected/excluded dependency for {CHARACTER}: {path}'
            fixture_inputs = {baseline, candidate} if selected else {baseline}
            try:
                module['check_input_freshness'](fixture_inputs, image)
            except ValueError as error:
                assert stale and 'sources changed after the build' in str(error), str(error)
            else:
                assert not stale, f'stale selected source accepted: {path}'

            os.utime(candidate, ns=(candidate_stat.st_atime_ns, image_mtime))
            module['check_input_freshness'](fixture_inputs, image)
    print('package freshness: real Ninja-selected inputs checked; temporary selected/excluded mtime fixtures reject only newer selected inputs')

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
    check_compiler_inputs()
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
