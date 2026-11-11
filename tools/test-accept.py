#!/usr/bin/env python3
"""Acceptance stage coverage, early failure and renderer compile isolation."""
from pathlib import Path
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import runpy
import subprocess
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
accept = runpy.run_path(str(ROOT / 'tools/accept.py'))
globals_ = accept['main'].__globals__


def commands(level):
    calls = []
    with patch.dict(globals_, preflight=lambda level: None,
                    run=lambda command, cwd=ROOT, timeout=600: calls.append((command, cwd))), \
            patch('sys.argv', ['accept.py', level]), redirect_stdout(io.StringIO()):
        assert accept['main']() == 0
    return calls


def check_stages():
    agent, native, build = (commands(level) for level in ('agent', 'native', 'build'))
    lint = commands('lint')
    # The build-only command installs plugin deps; local ci already installed them in agent.
    assert build[0] == (['npm', 'ci'], ROOT / 'openclaw-kubik')
    assert commands('ci') == lint + agent + native + build[1:]
    assert commands('host') == agent + native
    portable = commands('native-portable')
    changes = [(a, b) for a, b in zip(native, portable) if a != b]
    assert len(native) == len(portable) and len(changes) == 1
    assert changes[0][1][0] == [accept['sys'].executable, 'tools/test-frames.py', '--structure-only']
    assert commands('host-portable') == agent + portable
    assert not any('idf.py' in command for command, _ in agent)
    # All native suites still run exactly once; no cheaper representative subset.
    for name in accept['HOST_TESTS']:
        assert sum(f'tools/test-{name}.py' in command for command, _ in native) == 1


def check_preflight():
    def installed(name):
        return None if name in {'idf.py', 'clang', 'ninja'} else name
    with patch('shutil.which', installed), patch('os.geteuid', return_value=501), \
            patch.dict(globals_, run=lambda *args, **kwargs: None, require_node_runtime=lambda: None):
        accept['preflight']('agent')
        try:
            accept['preflight']('native')
        except RuntimeError as error:
            assert 'idf.py' in str(error)
        else:
            raise AssertionError('native accepted a missing SDK')
    with patch.dict(globals_, preflight=lambda level: None,
                    run=lambda *args, **kwargs: (_ for _ in ()).throw(subprocess.CalledProcessError(1, 'test'))), \
            patch('sys.argv', ['accept.py', 'ci']), redirect_stderr(io.StringIO()):
        assert accept['main']() == 1


def check_build_targets():
    calls = []
    with patch.dict(globals_, run=lambda command, cwd=ROOT, timeout=600: calls.append((command, timeout))):
        accept['build']()
    expected = [
        ('esp32c6', 'TESS', 'build'), ('esp32c6', 'PLUSH', 'build-plush'),
        ('esp32s3', 'TESS', 'build-s3'), ('esp32s3', 'PLUSH', 'build-s3-plush'),
    ]
    for index, (target, character, directory) in enumerate(expected):
        group = calls[index * 3:index * 3 + 3]
        assert group[0] == ([
            'idf.py', '-C', 'firmware', '-B', str(ROOT / 'firmware' / directory),
            f'-DIDF_TARGET={target}', f'-DKUBIK_CHARACTER={character}', 'reconfigure', 'build',
        ], 1200)
        for (command, timeout), name in zip(group[1:], ('package', 'kit')):
            assert command == [accept['sys'].executable, f'tools/test-{name}.py',
                               '--character', character, '--target', target]
            assert timeout == 600
    assert len(calls) == 13 and calls[-1][0][:2] == ['node', 'tools/test-install.mjs']
    pinned_sdk = json.loads((ROOT / 'openclaw-kubik/package.json').read_text())['devDependencies']['openclaw']
    assert calls[-1][0][-2:] == ['--openclaw', pinned_sdk], 'Clean installation must test the package SDK pin'
    assert len(set(accept['HOST_TESTS'])) == len(accept['HOST_TESTS'])
    assert {'board', 'target-memory'} <= set(accept['HOST_TESTS'])


def check_compile_flags():
    compile_object = runpy.run_path(str(ROOT / 'tools/test-render.py'))['compile_object']
    with tempfile.TemporaryDirectory(prefix='kubik-compile-isolation-') as directory:
        source = Path(directory) / 'value.c'
        source.write_text('int value(void) { return VALUE; }\n')
        fixture = Path(directory) / 'main.c'
        fixture.write_text('#include <stdio.h>\nint value(void); int main(void) { printf("%d", value()); }\n')
        objects = {}
        first = compile_object(directory, str(source), ['cc', '-DVALUE=1'], objects)
        assert first == compile_object(directory, str(source), ['cc', '-DVALUE=1'], objects)
        second = compile_object(directory, str(source), ['cc', '-DVALUE=2'], objects)
        assert first != second and len(objects) == 2
        for unit, expected in ((first, b'1'), (second, b'2')):
            exe = str(Path(directory) / 'check')
            subprocess.run(['cc', str(fixture), unit, '-o', exe], check=True)
            assert subprocess.check_output([exe]) == expected


def check_firmware_profiles():
    cmake = (ROOT / 'firmware/main/CMakeLists.txt').read_text()
    start = cmake.index('if(IDF_TARGET STREQUAL "esp32c6" AND KUBIK_CHARACTER STREQUAL "PLUSH")')
    logs = cmake[start:cmake.index('endif()', start) + len('endif()')]
    start = cmake.index('set(MAIN_OPTIMIZATION')
    end = cmake.index('# Tess', start)
    flags = cmake[start:end]
    start = cmake.index('# C6 keeps short DMA strips')
    hot = cmake[start:cmake.index('endif()', start) + len('endif()')]
    start = cmake.index('# Configuration and diagnostic handlers are cold paths;')
    cold = cmake[start:cmake.index('# Plush has a smaller app partition:', start)]
    bound = 'set_property(SOURCE render_text.c APPEND PROPERTY COMPILE_OPTIONS -Werror=vla)'
    assert bound in cmake
    with tempfile.TemporaryDirectory(prefix='kubik-firmware-profiles-') as temporary:
        root = Path(temporary)
        script = root / 'profile.cmake'
        script.write_text("""set(COMPONENT_LIB main)
function(idf_component_get_property output component property)
    set(${output} ${component} PARENT_SCOPE)
endfunction()
function(target_compile_definitions target)
    file(APPEND "${RESULT}" "sdk:${target}:${ARGN}\\n")
endfunction()
function(set_source_files_properties)
    list(GET ARGN -1 optimization)
    if(optimization STREQUAL "-O3")
        set(profile hot)
    else()
        set(profile cold)
    endif()
    file(APPEND "${RESULT}" "${profile}:${ARGN}\\n")
endfunction()
function(set_property)
    file(APPEND "${RESULT}" "bound:${ARGN}\\n")
endfunction()
function(target_compile_options target)
    file(APPEND "${RESULT}" "app:${target}:${ARGN}\\n")
endfunction()
""" + logs + '\n' + hot + '\n' + bound + '\n' + flags + '\n' + cold)
        for target in ('esp32c6', 'esp32s3'):
            for character in ('TESS', 'PLUSH'):
                result = root / (target + character)
                subprocess.run(['cmake', f'-DIDF_TARGET={target}', f'-DKUBIK_CHARACTER={character}',
                                f'-DRESULT={result}', '-P', str(script)], check=True)
                lines = result.read_text().splitlines()
                verify_firmware_profile(lines, target, character)


def verify_firmware_profile(lines, target, character):
    small = target == 'esp32c6' and character == 'PLUSH'
    app = [line for line in lines if line.startswith('app:')]
    assert len(app) == 1 and app[0].startswith('app:main:PRIVATE;' + ('-Os' if small else '-O2') + ';')
    cold = [line for line in lines if line.startswith('cold:')]
    expected = '-Os;-msave-restore' if small else '-Os'
    assert len(cold) == 1 and cold[0].endswith('PROPERTIES;COMPILE_OPTIONS;' + expected)
    verify_sdk_profile(lines, small)
    verify_fast_profile(lines, target == 'esp32c6' and character == 'TESS')
    assert 'bound:SOURCE;render_text.c;APPEND;PROPERTY;COMPILE_OPTIONS;-Werror=vla' in lines


def verify_sdk_profile(lines, small):
    sdk = [line for line in lines if line.startswith('sdk:')]
    assert bool(sdk) is small
    assert all(line.endswith(':PRIVATE;LOG_LOCAL_LEVEL=2') for line in sdk)
    assert not any(line.startswith(('sdk:main:', 'sdk:esp-tls:')) for line in sdk)
    if small:
        assert any(line.startswith('sdk:bt:') for line in sdk)


def verify_fast_profile(lines, expected):
    fast = [line for line in lines if line.startswith('hot:')]
    assert bool(fast) is expected
    assert all(line.endswith('PROPERTIES;COMPILE_OPTIONS;-O3') for line in fast)


def check_shader_stack_guard():
    with tempfile.TemporaryDirectory(prefix='kubik-shader-stack-') as temporary:
        root = Path(temporary)
        for extent, valid in (('480', True), ('width', False)):
            source = root / 'shader.c'
            source.write_text('#include <stdint.h>\nvoid icon(int width, uint16_t *out) {\n'
                              f'uint16_t xq[{extent}];\n'
                              'for (int i=0;i<width;i++) xq[i]=(uint16_t)i;\n'
                              'for (int i=0;i<width;i++) out[i]=xq[i]; }\n')
            result = subprocess.run(['cc', '-std=c11', '-O3', '-Werror=vla', '-c', str(source),
                                     '-o', str(root / 'shader.o')], text=True, capture_output=True)
            assert (result.returncode == 0) is valid, result.stderr
            if not valid:
                assert 'variable length array' in result.stderr.lower()


def check_node_runtime():
    supported = accept['node_supported']
    specification = json.loads((ROOT / 'openclaw-kubik/package.json').read_text())['engines']['node']
    for version, expected in [('24.16.0', False), ('24.22.0', False), ('24.22.1', True),
                              ('24.23.0', True), ('25.0.0', False), ('26.12.0', False),
                              ('26.12.1', True), ('not-a-version', False)]:
        assert supported(version, specification) is expected, version
    assert supported((ROOT / '.node-version').read_text().strip(), specification)
    try:
        supported('24.22.1', '*')
    except RuntimeError:
        pass
    else:
        raise AssertionError('An unknown engines syntax must not silently accept a runtime')


check_stages()
check_preflight()
check_build_targets()
check_compile_flags()
check_firmware_profiles()
check_shader_stack_guard()
check_node_runtime()
print('acceptance: four target/character builds, split/full coverage, missing SDK, failure and compiler flag isolation passed')
