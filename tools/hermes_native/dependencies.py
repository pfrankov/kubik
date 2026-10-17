"""Verify Kubik's dependency declaration through the pinned Hermes package manager."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def check_pm(source):
    from pm.client import sync_venv
    from pm.environments import selected_venv, venv_python
    from pm.plugin_declarations import read_python_declaration
    from pm.workspace import enabled_member_dirs

    home = Path(os.environ['HERMES_HOME'])
    home.mkdir(mode=0o700, parents=True)
    (home / 'config.yaml').write_text('plugins:\n  isolation: in_process\n')
    subprocess.run([sys.executable, str(ROOT / 'tools/install-hermes.py'),
                    '--home', str(home)], check=True)
    expected = tuple(line for line in (ROOT / 'hermes-kubik/requirements.txt').read_text().splitlines()
                     if line and not line.startswith('#'))
    assert read_python_declaration(home / 'plugins/kubik').install_requirements == expected
    assert home / 'plugins/kubik' in enabled_member_dirs(), 'PM omitted the installed Kubik manifest'
    upstream_lock = (source / 'uv.lock').read_bytes()
    sync_venv(['telegram'], explicit=True)
    for repair in (False, True):
        sync_venv(explicit=True, repair=repair)
        python = venv_python(selected_venv(source))
        subprocess.run([str(python), '-c',
                        'import tornado; print("PM Tornado:", tornado.version, flush=True); '
                        'assert (6, 5, 9) <= tornado.version_info[:3] < (7, 0, 0)'], check=True)
        assert (source / 'uv.lock').read_bytes() == upstream_lock, 'PM changed the upstream lock'
    print('PASS: installed manifest, native PM sync/repair retain the Tornado floor without changing upstream uv.lock', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    source = args.source.resolve()
    if args.worker:
        check_pm(source)
        return
    with tempfile.TemporaryDirectory(prefix='kubik-hermes-dependencies-') as directory:
        env = {name: value for name, value in os.environ.items() if not name.startswith('HERMES_')}
        env.update(HOME=directory, HERMES_HOME=str(Path(directory) / 'hermes'),
                   PYTHONPATH=str(source), XDG_CONFIG_HOME=str(Path(directory) / 'config'),
                   XDG_CACHE_HOME=str(Path(directory) / 'cache'), XDG_DATA_HOME=str(Path(directory) / 'data'))
        subprocess.run([str(source / '.venv/bin/python'), __file__, '--source', str(source), '--worker'],
                       env=env, cwd=ROOT, timeout=600, check=True)


if __name__ == '__main__':
    main()
