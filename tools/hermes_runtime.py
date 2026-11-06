"""Prepare Kubik dependencies with Hermes PM before the atomic plugin installer."""
import contextlib
import os
from pathlib import Path
import subprocess
import tempfile


@contextlib.contextmanager
def profile_environment(home):
    previous = os.environ.get('HERMES_HOME')
    os.environ['HERMES_HOME'] = str(home)
    try:
        yield
    finally:
        if previous is None:
            os.environ.pop('HERMES_HOME', None)
        else:
            os.environ['HERMES_HOME'] = previous


def configuration_snapshot(homes):
    result = {}
    for home in homes:
        path = home / 'config.yaml'
        result[path] = path.read_bytes() if path.exists() else None
    return result


def candidate_files(candidate):
    return {path.name: path.read_bytes() for path in candidate.iterdir() if path.is_file()}


def prepare_runtime(home, before, after, stage, command):
    """Only PM publishes dependencies; existing plugin/config stay intact until install."""
    with profile_environment(home):
        _prepare_profile(home, before, after, stage, command)


def _prepare_profile(home, before, after, stage, command):
    from pm import paths
    from pm.client import sync_venv
    from pm.environments import selected_venv, venv_command
    from pm.plugin_inputs import Members
    from pm.plugins_state import dependency_homes
    from pm.workspace import enabled_member_dirs
    from ruamel.yaml import YAML

    homes = dependency_homes()
    if home not in [path.resolve() for path in homes]:
        raise ValueError('--home must name the Hermes default home or a live profile')
    snapshot = configuration_snapshot(homes)
    if snapshot.get(home / 'config.yaml') != before:
        raise ValueError('config.yaml changed before dependency preparation')
    selection = YAML(typ='safe').load(after)['plugins']
    members = enabled_member_dirs(proposed_home=home, enabled=selection['enabled'],
                                  disabled=selection['disabled'])
    sources = {path.resolve(): path for path in members}
    with tempfile.TemporaryDirectory(prefix='kubik-hermes-candidate-') as directory:
        candidate = Path(directory) / 'kubik'
        stage(candidate)
        original = candidate_files(candidate)
        sources[home / 'plugins/kubik'] = candidate
        print('Preparing Kubik dependencies through Hermes PM; existing plugins and settings remain in place.',
              flush=True)
        try:
            sync_venv(explicit=True, plugins=Members(sources))
        except Exception as error:
            raise ValueError(f'Hermes PM dependency preparation failed: {error}') from error
        if configuration_snapshot(homes) != snapshot or dependency_homes() != homes:
            raise ValueError('Hermes profile selection changed during preparation; rerun before installing')
        refreshed = Path(directory) / 'recheck'
        stage(refreshed)
        if candidate_files(refreshed) != original:
            raise ValueError('Kubik source changed during preparation; rerun before installing')
        root = paths.repo_root()
        invocation = venv_command(root, selected_venv(root)) + command
        try:
            result = subprocess.run(invocation, check=False, timeout=120)
        except subprocess.TimeoutExpired as error:
            raise ValueError('Prepared Hermes runtime, but atomic installation exceeded 120 seconds') from error
        if result.returncode:
            raise ValueError(f'Prepared Hermes runtime, but Kubik installation exited {result.returncode}')
