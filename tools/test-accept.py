#!/usr/bin/env python3
"""Acceptance stage coverage, early failure and renderer compile isolation."""
from pathlib import Path
from contextlib import redirect_stderr, redirect_stdout
import io
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
            patch.dict(globals_, run=lambda *args, **kwargs: None):
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


check_stages()
check_preflight()
check_compile_flags()
print('acceptance: split/full coverage, missing SDK, failure and compiler flag isolation passed')
