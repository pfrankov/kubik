#!/usr/bin/env python3
"""The real CI final gate rejects any incomplete acceptance stage."""
from itertools import product
import os
from pathlib import Path
import subprocess
import yaml

ROOT = Path(__file__).resolve().parent.parent
prepare = yaml.load((ROOT / '.github/actions/prepare-ci/action.yml').read_text(), Loader=yaml.BaseLoader)
steps = prepare['runs']['steps']
sdk_install = next(i for i, step in enumerate(steps) if '/esp-idf/install.sh' in step.get('run', ''))
path_exports = [i for i, step in enumerate(steps) if 'GITHUB_PATH' in step.get('run', '')]
assert path_exports and all(i > sdk_install for i in path_exports), 'SDK install must not use the uv virtualenv Python'
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
