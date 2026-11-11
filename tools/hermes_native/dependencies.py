"""Verify Kubik's dependency declaration through the pinned Hermes package manager."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def legacy_installation(home):
    """The real upgrade starts in a selected runtime that cannot import the new dependency."""
    old = home / 'plugins/kubik'
    old.mkdir(mode=0o700, parents=True)
    (old / 'plugin.yaml').write_text('name: kubik\nkind: platform\nmanifest_version: 2\nversion: 0.2.1\n'
                                   'python_dependencies: ["aiohttp==3.14.3", "tornado>=6.5.9,<7"]\n')
    (old / 'requirements.txt').write_text('aiohttp==3.14.3\ntornado>=6.5.9,<7\n')
    (old / 'old.py').write_text('# Superseded installation; removed by the atomic replacement.\n')
    (home / 'config.yaml').write_text('# preserved\nmodel: existing-agent\nplugins:\n'
                                    '  enabled: [kubik]\n  disabled: []\n'
                                    'platforms:\n  telegram: {enabled: true}\n')
    profile = home / 'profiles/other'
    member = profile / 'plugins/other-plugin'
    member.mkdir(parents=True)
    (profile / 'config.yaml').write_text('plugins:\n  enabled: [other-plugin]\n  disabled: []\n')
    (member / 'plugin.yaml').write_text('name: other-plugin\nkind: tool\nmanifest_version: 2\nversion: 1.0.0\n'
                                       'python_dependencies: ["humanize==4.13.0"]\n')
    return member, (profile / 'config.yaml').read_bytes(), (home / 'config.yaml').read_bytes()


def assert_runtime(command, wasm):
    subprocess.run(command + ['-c',
                   'import sys, tornado, wasmtime, humanize, regex; from importlib.metadata import version; '
                   'print("PM Tornado:", tornado.version, "Wasmtime:", version("wasmtime"), flush=True); '
                   'assert (6, 5, 9) <= tornado.version_info[:3] < (7, 0, 0); '
                   'assert version("wasmtime") == "50.0.1"; '
                   'assert version("regex") == "2026.9.10"; '
                   'assert version("humanize") == "4.13.0"; '
                   'wasmtime.Module.from_file(wasmtime.Engine(), sys.argv[1])', str(wasm)], check=True)


def check_pm(source):
    from pm.client import sync_venv
    from pm.environments import selected_venv, venv_command
    from pm.plugin_declarations import read_python_declaration
    from pm.workspace import enabled_member_dirs

    home = Path(os.environ['HERMES_HOME'])
    home.mkdir(mode=0o700, parents=True)
    member, other_config, old_config = legacy_installation(home)
    upstream_lock = (source / 'uv.lock').read_bytes()
    sync_venv(['telegram'], explicit=True)
    selected = venv_command(source, selected_venv(source))
    subprocess.run(selected + ['-c', 'import importlib.util, humanize; '
                    'assert importlib.util.find_spec("wasmtime") is None; '
                    'print("BEFORE: actual selected runtime has no Wasmtime; other profile is present", flush=True)'],
                   check=True)
    subprocess.run(selected + [str(ROOT / 'tools/install-hermes.py'), '--home', str(home), '--prepare-runtime'],
                   check=True)
    expected = tuple(line for line in (ROOT / 'hermes-kubik/requirements.txt').read_text().splitlines()
                     if line and not line.startswith('#'))
    assert read_python_declaration(home / 'plugins/kubik').install_requirements == expected
    assert home / 'plugins/kubik' in enabled_member_dirs(), 'PM omitted the installed Kubik manifest'
    assert not (home / 'plugins/kubik/old.py').exists(), 'Obsolete plugin source remained installed'
    assert (home / 'config.before-kubik.yaml').read_bytes() == old_config
    assert '# preserved' in (home / 'config.yaml').read_text()
    assert 'existing-agent' in (home / 'config.yaml').read_text()
    for repair in (False, True):
        sync_venv(explicit=True, repair=repair)
        assert_runtime(venv_command(source, selected_venv(source)), home / 'plugins/kubik/speex-aec.wasm')
        assert (source / 'uv.lock').read_bytes() == upstream_lock, 'PM changed the upstream lock'
        assert (member.parent.parent / 'config.yaml').read_bytes() == other_config
        assert member in enabled_member_dirs(), 'PM dropped another enabled profile'
    print('PASS: upgrade from selected runtime without Wasmtime, atomic plugin replacement, other-profile union; '
          'PM sync/repair retain Tornado and load the pinned AEC without changing upstream lock/config', flush=True)


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
