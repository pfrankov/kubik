#!/usr/bin/env python3
"""Run the real pinned Hermes Gateway in a temporary home, with loopback-only test providers."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
PIN = json.loads((ROOT / 'tools/hermes_native/upstream.json').read_text())


def prepare_source(destination):
    """Create only a new disposable checkout; never update the user's Hermes installation."""
    if destination.exists():
        raise ValueError('--prepare destination must not exist')
    subprocess.run(['git', 'clone', '--quiet', '--depth', '1', '--branch', PIN['tag'],
                    PIN['repository'], str(destination)], check=True)
    verify_source(destination)
    subprocess.run(['uv', '--no-config', 'sync', '--directory', str(destination), '--frozen',
                    '--no-dev', '--python', PIN['python']], check=True)
    python = destination / '.venv/bin/python'
    subprocess.run(['uv', 'pip', 'install', '--python', str(python),
                    '-r', str(ROOT / 'hermes-kubik/requirements.txt')], check=True)


def verify_source(source):
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != PIN['commit']:
        raise ValueError(f'Expected Hermes {PIN["tag"]} at {PIN["commit"]}, got {revision}')
    changed = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--untracked-files=no'], text=True)
    if changed.strip():
        raise ValueError('Hermes source has tracked changes; use a clean disposable checkout')


def run_contract(source):
    verify_source(source)
    python = source / '.venv/bin/python'
    version = subprocess.check_output([str(python), '-c', 'import sys; print(".".join(map(str, sys.version_info[:3])))'], text=True).strip()
    if version != PIN['python']:
        raise ValueError(f'Native acceptance requires Python {PIN["python"]}; got {version}')
    if not shutil.which('ffmpeg'):
        raise ValueError('Install ffmpeg before running the native speech contract')
    with tempfile.TemporaryDirectory(prefix='kubik-hermes-native-') as temporary:
        work = Path(temporary)
        # Do not inherit user tokens, plugin locations, proxy settings or existing homes.
        env = {name: os.environ[name] for name in ('PATH', 'SYSTEMROOT', 'LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH') if name in os.environ}
        env.update(HOME=str(work), HERMES_HOME=str(work / 'hermes'),
                   PYTHONPATH=str(source), PYTHONDONTWRITEBYTECODE='1', PYTHONUNBUFFERED='1',
                   XDG_CONFIG_HOME=str(work / 'config'), XDG_CACHE_HOME=str(work / 'cache'),
                   XDG_DATA_HOME=str(work / 'data'), NO_PROXY='127.0.0.1,localhost,::1',
                   LANG='C.UTF-8')
        subprocess.run([str(python), str(ROOT / 'tools/hermes_native/contract.py')],
                       cwd=ROOT, env=env, timeout=300, check=True)
    print(f'Hermes native acceptance passed: {PIN["tag"]} / {PIN["commit"]} / Python {version}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--prepare', type=Path, help='Download and prepare a NEW disposable upstream checkout, then test it')
    group.add_argument('--source', type=Path, help='Test an already prepared immutable upstream checkout without installing anything')
    args = parser.parse_args()
    source = (args.prepare or args.source).expanduser().resolve()
    try:
        if args.prepare:
            prepare_source(source)
        run_contract(source)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f'Hermes native acceptance failed: {error}\n')


if __name__ == '__main__':
    main()
