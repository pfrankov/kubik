#!/usr/bin/env python3
"""The real CI final gate rejects any incomplete acceptance stage."""
from itertools import product
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import re
import runpy
import shlex
import subprocess
import tempfile
from unittest.mock import patch
import yaml

ROOT = Path(__file__).resolve().parent.parent
prepare = yaml.load((ROOT / '.github/actions/prepare-ci/action.yml').read_text(), Loader=yaml.BaseLoader)
steps = prepare['runs']['steps']
sdk_install = next(i for i, step in enumerate(steps) if '/esp-idf/install.sh' in step.get('run', ''))
path_exports = [i for i, step in enumerate(steps) if 'GITHUB_PATH' in step.get('run', '')]
assert path_exports and all(i > sdk_install for i in path_exports), 'SDK install must not use the uv virtualenv Python'
uv_lock = 'tools/ci-uv.requirements.txt'
requirement = ' '.join(line for line in (ROOT / uv_lock).read_text().splitlines() if not line.startswith('#')).replace('\\', '')
package, *hashes = shlex.split(requirement)
assert re.fullmatch(r'uv==\d+\.\d+\.\d+', package), 'One exact uv version must own the bootstrap'
assert len(hashes) == 3 and len(set(hashes)) == 3, 'The three CI platform wheels must be pinned'
assert all(re.fullmatch(r'--hash=sha256:[0-9a-f]{64}', value) for value in hashes)
hermes = yaml.load((ROOT / '.github/workflows/hermes-native.yml').read_text(), Loader=yaml.BaseLoader)
for ci_steps in (steps, hermes['jobs']['hermes']['steps']):
    installs = [line for step in ci_steps for line in step.get('run', '').splitlines() if uv_lock in line]
    assert len(installs) == 1, 'Each CI path must use the shared uv lock'
    args = shlex.split(installs[0])
    assert {'install', '--require-hashes', '--only-binary=:all:'} <= set(args), 'Bootstrap must verify wheel hashes before install'
    assert args[args.index('-r') + 1] == uv_lock
assert uv_lock in next(step['env']['DEPENDENCIES'] for step in steps if step.get('id') == 'keys'), 'uv updates must invalidate the dependency cache'
workflow = yaml.load((ROOT / '.github/workflows/acceptance.yml').read_text(), Loader=yaml.BaseLoader)
checks = workflow['jobs']['checks']
assert checks['needs'] == 'lint'
matrix = checks['strategy']['matrix']
assert set(matrix['os']) == {'macos-15', 'ubuntu-24.04'}
assert set(matrix['stage']) == {'agent', 'native', 'build'}
assert len(matrix['os']) * len(matrix['stage']) == 6
gate = workflow['jobs']['acceptance']
assert gate['needs'] == ['lint', 'checks'] and gate['if'] == 'always()'
script = gate['steps'][0]['run']
for lint, checks in product(('success', 'failure', 'cancelled', 'skipped'), repeat=2):
    result = subprocess.run(['bash', '-e', '-c', script], env={**os.environ, 'LINT_RESULT': lint, 'CHECKS_RESULT': checks})
    assert (result.returncode == 0) == (lint == checks == 'success'), (lint, checks)
print('CI: all six stages required; final gate rejects all 15 failure/cancel/skip combinations')


def check_apt_result(installer, failure):
    calls = []

    def run(command, **options):
        assert options['check']
        calls.append(command)
        if len(calls) == 1:
            assert options == {'input': 'https://archive.ubuntu.com/ubuntu/\n', 'text': True, 'check': True}
        else:
            assert options == {'check': True}
        if failure and len(calls) - 1 == failure[0]:
            raise subprocess.CalledProcessError(failure[1], command)

    with patch('subprocess.run', run), patch.object(Path, 'read_text', return_value='URIs: mirror+file:/etc/apt/apt-mirrors.txt'), redirect_stdout(io.StringIO()):
        try:
            installer(['ffmpeg'])
        except subprocess.CalledProcessError as error:
            assert failure and error.returncode == failure[1]
        else:
            assert failure is None
    assert len(calls) == (failure[0] + 1 if failure else 3)
    assert calls[0] == ['sudo', 'timeout', '--kill-after=5s', '10s', 'tee', '/etc/apt/apt-mirrors.txt']
    for command in calls[1:]:
        assert command[:5] == ['sudo', 'timeout', '--kill-after=5s', '180s', 'apt-get']
        assert all(option in command for option in ('Acquire::Retries=1', 'Acquire::http::Timeout=15',
                                                    'Acquire::https::Timeout=15', 'DPkg::Lock::Timeout=30'))
    if len(calls) > 1:
        assert calls[1][-2:] == ['update', '--error-on=any']
    if len(calls) == 3:
        assert calls[2][-3:] == ['install', '-y', 'ffmpeg']


def check_apt():
    installer = runpy.run_path(str(ROOT / 'tools/ci-apt.py'))['install']
    for failure in (None, (0, 1), (0, 124), (1, 100), (1, 124), (2, 100), (2, 124)):
        check_apt_result(installer, failure)
    with patch.object(Path, 'read_text', return_value='URIs: https://unexpected.example/ubuntu/'), patch('subprocess.run') as run:
        try:
            installer(['ffmpeg'])
        except RuntimeError as error:
            assert str(error) == 'Unexpected Ubuntu CI source layout'
        else:
            raise AssertionError('Unknown Ubuntu source layout accepted')
        run.assert_not_called()
    for packages in ([], ['--allow-unauthenticated'], ['ffmpeg;true']):
        with patch('subprocess.run') as run:
            try:
                installer(packages)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid APT arguments accepted')
            run.assert_not_called()
    for ci_steps in (steps, hermes['jobs']['hermes']['steps']):
        scripts = '\n'.join(step.get('run', '') for step in ci_steps)
        assert scripts.count('tools/ci-apt.py') == 1 and 'apt-get' not in scripts


check_apt()
print('CI: official Ubuntu archive, expected layout, bounded mirror setup/update/install and fatal failures passed')


def check_sdk_bootstrap():
    script = steps[sdk_install]['run']
    packages = 'flex bison gperf ccache libffi-dev libssl-dev libusb-1.0-0'
    expected = ['cmake --version', 'ninja --version', f'python3 tools/ci-apt.py {packages}', 'install.sh esp32c6']
    for failing in (None, 'cmake', 'ninja', 'python3'):
        with tempfile.TemporaryDirectory(prefix='kubik-ci-bootstrap-') as directory:
            root = Path(directory)
            sdk = root / 'esp-idf'
            sdk.mkdir()
            trace = root / 'trace'
            for name in ('cmake', 'ninja', 'python3', 'install.sh'):
                target = (sdk if name == 'install.sh' else root) / name
                target.write_text(f'#!/bin/sh\nprintf "%s\\n" "{name} $*" >> "$KUBIK_CI_TRACE"\n'
                                  f'exit {127 if name == failing else 0}\n')
                target.chmod(0o755)
            env = {**os.environ, 'PATH': f'{root}:{os.environ["PATH"]}', 'RUNNER_OS': 'Linux',
                   'RUNNER_TEMP': str(root), 'KUBIK_CI_TRACE': str(trace)}
            result = subprocess.run(['bash', '-e', '-c', script], env=env, timeout=10)
            count = ('cmake', 'ninja', 'python3').index(failing) + 1 if failing else len(expected)
            assert trace.read_text().splitlines() == expected[:count]
            assert result.returncode == (127 if failing else 0)


check_sdk_bootstrap()
print('CI: Ubuntu uses image CMake/Ninja; missing tools or APT failure stop SDK preparation')
