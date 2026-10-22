#!/usr/bin/env python3
"""The real CI final gate rejects any incomplete acceptance stage."""
from itertools import product
import os
from pathlib import Path
import re
import shlex
import subprocess
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
