#!/usr/bin/env python3
"""Install Kubik into an existing Hermes home; never restart services or change providers."""

import argparse
from collections.abc import MutableMapping
import contextlib
import fcntl
import importlib.util
import io
import ipaddress
import json
import os
from pathlib import Path
import shutil
import shlex
import sys
import uuid


def require_runtime():
    if sys.version_info[:2] != (3, 14):
        raise ValueError('Use the Python 3.14 interpreter of Hermes 0.22.2, not system Python')
    missing = []
    for name in ('hermes_cli', 'gateway', 'aiohttp', 'cryptography', 'ruamel.yaml'):
        try:
            found = importlib.util.find_spec(name)
        except (ImportError, ValueError):
            found = None
        if found is None:
            missing.append(name)
    if missing:
        raise ValueError('Missing in this Hermes environment: ' + ', '.join(missing)
                         + '. Install hermes-kubik/requirements.txt with uv pip --python pointing to this interpreter')


def mapping(parent, key):
    value = parent.setdefault(key, {})
    if not isinstance(value, MutableMapping):
        raise ValueError(f'Hermes {key} must be a mapping')
    return value


def plugin_list(plugins, key):
    value = plugins.setdefault(key, [])
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise ValueError(f'Hermes plugins.{key} must be a list of names')
    return value


def configure_listener(extra, port, bind):
    chosen_port = extra.get('port', 18793) if port is None else port
    if type(chosen_port) is not int or not 1024 <= chosen_port <= 65535:
        raise ValueError('Kubik port must be an integer from 1024 to 65535')
    chosen_bind = extra.get('bind', '0.0.0.0') if bind is None else bind
    if not isinstance(chosen_bind, str):
        raise ValueError('Kubik bind must be a listener IP address')
    ipaddress.ip_address(chosen_bind)
    extra.update(port=chosen_port, bind=chosen_bind)
    return chosen_port


def configuration(path, port, bind):
    from ruamel.yaml import YAML
    from ruamel.yaml.error import YAMLError
    if path.is_symlink() or not path.is_file():
        raise ValueError('Set --home to an existing Hermes home with a regular config.yaml')
    before = path.read_bytes()
    yaml = YAML()
    yaml.preserve_quotes = True
    try:
        config = yaml.load(before.decode('utf-8'))
    except YAMLError as error:
        raise ValueError('Hermes config.yaml is not valid YAML') from error
    if not isinstance(config, MutableMapping):
        raise ValueError('Hermes config.yaml must contain a mapping')
    plugins = mapping(config, 'plugins')
    if plugins.get('isolation', 'in_process') != 'in_process':
        raise ValueError('Kubik requires native in-process platform plugins; plugins.isolation was not changed')
    enabled, disabled = plugin_list(plugins, 'enabled'), plugin_list(plugins, 'disabled')
    if 'kubik-platform' not in enabled:
        enabled.append('kubik-platform')
    plugins['disabled'] = [name for name in disabled if name != 'kubik-platform']
    platform = mapping(mapping(config, 'platforms'), 'kubik')
    extra = mapping(platform, 'extra')
    chosen_port = configure_listener(extra, port, bind)
    platform['enabled'] = True
    output = io.StringIO()
    yaml.dump(config, output)
    return before, output.getvalue().encode('utf-8'), chosen_port


def regular_destination(path, directory=False):
    if path.is_symlink() or (path.exists() and not (path.is_dir() if directory else path.is_file())):
        raise ValueError(f'Refusing a symlink or unexpected file type: {path.name}')


def private_write(path, data):
    with path.open('xb') as stream:
        os.fchmod(stream.fileno(), 0o600)
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def sync_directory(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def write_transaction(home, state):
    temporary = home / '.kubik-install.tmp'
    regular_destination(temporary)
    temporary.unlink(missing_ok=True)
    private_write(temporary, json.dumps(state).encode())
    temporary.replace(home / '.kubik-install.json')
    sync_directory(home)


def transaction_directory(parent, name, prefix):
    if not isinstance(name, str) or Path(name).name != name or not name.startswith(prefix):
        raise ValueError('Invalid Kubik installation recovery path')
    path = parent / name
    regular_destination(path, directory=True)
    return path


def same_identity(path, identity):
    return path.exists() and [path.stat().st_dev, path.stat().st_ino] == identity


def valid_identity(value):
    return (isinstance(value, list) and len(value) == 2
            and all(type(item) is int and item >= 0 for item in value))


def rollback_plugin(destination, staged, previous, state):
    regular_destination(destination, directory=True)
    regular_destination(staged, directory=True)
    regular_destination(previous, directory=True)
    if staged.exists() and not same_identity(staged, state['identity']):
        raise ValueError('Staged Kubik plugin changed; recovery files retained')
    if previous.exists():
        if not same_identity(previous, state['previous_identity']):
            raise ValueError('Previous Kubik plugin changed; recovery files retained')
        if destination.exists():
            if not same_identity(destination, state['identity']):
                raise ValueError('Kubik plugin changed during recovery; previous plugin retained')
            destination.rename(staged)
            sync_directory(staged.parent)
        previous.rename(destination)
        sync_directory(previous.parent)
        sync_directory(destination.parent)
    elif same_identity(destination, state['identity']):
        if state['had_plugin']:
            raise ValueError('Previous Kubik plugin missing; recovery files retained')
        destination.rename(staged)
        sync_directory(staged.parent)
        sync_directory(destination.parent)
    elif not staged.exists() and not destination.exists() and state['had_plugin']:
        raise ValueError('Kubik plugin missing; recovery files retained')


def preserve_observed(home, observed, config):
    if not observed.exists() or (config.exists() and os.path.samefile(config, observed)):
        return
    for name in ('config.before-kubik.yaml', 'config.before-kubik-conflict.yaml'):
        backup = home / name
        regular_destination(backup)
        if not backup.exists():
            private_write(backup, observed.read_bytes())
            sync_directory(home)
            return
        if backup.read_bytes() == observed.read_bytes():
            backup.chmod(0o600)
            return
    raise ValueError('Configuration conflict backup occupied; recovery files retained')


def read_transaction(home):
    journal = home / '.kubik-install.json'
    regular_destination(journal)
    if not journal.exists():
        return None
    if journal.stat().st_size > 4096:
        raise ValueError('Kubik recovery journal is too large')
    state = json.loads(journal.read_text())
    if (not isinstance(state, dict) or state.get('phase') not in ('staging', 'prepared', 'committed', 'rolled_back')
            or type(state.get('had_plugin')) is not bool
            or (state.get('had_plugin') and not valid_identity(state.get('previous_identity')))
            or (state.get('phase') == 'prepared' and not valid_identity(state.get('identity')))):
        raise ValueError('Invalid Kubik installation recovery state')
    return state


def transaction_committed(state, config, prepared, destination):
    if state['phase'] == 'committed':
        return True
    return (state['phase'] == 'prepared' and config.is_file() and not config.is_symlink()
            and prepared.is_file() and os.path.samefile(config, prepared)
            and same_identity(destination, state['identity']))


def rollback_transaction(home, state, plugin_work, config_work):
    config, observed = home / 'config.yaml', config_work / 'observed-config.yaml'
    if not config.exists() and observed.exists():
        os.link(observed, config, follow_symlinks=False)
    rollback_plugin(home / 'plugins/kubik', plugin_work / 'plugin', plugin_work / 'previous', state)
    preserve_observed(home, observed, config)
    sync_directory(home / 'plugins')
    sync_directory(home)


def recover_installation(home):
    state = read_transaction(home)
    if state is None:
        return
    plugin_work = transaction_directory(home / 'plugins', state.get('plugin'), '.kubik-plugin-')
    config_work = transaction_directory(home, state.get('config'), '.kubik-config-')
    destination, config = home / 'plugins/kubik', home / 'config.yaml'
    prepared, observed = config_work / 'config.yaml', config_work / 'observed-config.yaml'
    regular_destination(prepared)
    regular_destination(observed)
    committed = transaction_committed(state, config, prepared, destination)
    if state['phase'] == 'prepared' and not committed:
        rollback_transaction(home, state, plugin_work, config_work)
    if state['phase'] in ('prepared', 'staging'):
        write_transaction(home, {**state, 'phase': 'committed' if committed else 'rolled_back'})
    for directory in (plugin_work, config_work):
        if directory.exists():
            shutil.rmtree(directory)
            sync_directory(directory.parent)
    (home / '.kubik-install.json').unlink()
    sync_directory(home)


@contextlib.contextmanager
def installation_work(home, destination):
    suffix = uuid.uuid4().hex
    plugin_work, config_work = destination.parent / ('.kubik-plugin-' + suffix), home / ('.kubik-config-' + suffix)
    previous = destination.stat() if destination.exists() else None
    state = {'plugin': plugin_work.name, 'config': config_work.name, 'phase': 'staging',
             'had_plugin': previous is not None,
             'previous_identity': [previous.st_dev, previous.st_ino] if previous else None}
    write_transaction(home, state)
    try:
        plugin_work.mkdir(mode=0o700)
        config_work.mkdir(mode=0o700)
        yield plugin_work, config_work, state
    finally:
        # A failed recovery keeps its evidence; SIGKILL leaves the journal for the next install.
        recover_installation(home)


@contextlib.contextmanager
def installation_lock(home):
    descriptor = os.open(home / '.kubik-install.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        os.fchmod(descriptor, 0o600)
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(descriptor)


def promotion_state(staged, destination, expected):
    had_plugin = destination.exists()
    candidate = staged.stat()
    original = destination.stat() if had_plugin else None
    current = [original.st_dev, original.st_ino] if original else None
    if expected and (had_plugin != expected['had_plugin'] or current != expected['previous_identity']):
        raise ValueError('Kubik plugin changed during staging; no plugin was overwritten')
    if expected and [candidate.st_dev, candidate.st_ino] != expected['identity']:
        raise ValueError('Staged Kubik plugin changed; recovery files retained')
    return expected or {'had_plugin': had_plugin, 'identity': [candidate.st_dev, candidate.st_ino],
                        'previous_identity': current}


def promote(staged, destination, config_path, prepared_config, before, previous, expected=None):
    state = promotion_state(staged, destination, expected)
    if state['had_plugin']:
        destination.rename(previous)
        sync_directory(destination.parent)
        sync_directory(previous.parent)
        if not same_identity(previous, state['previous_identity']):
            raise ValueError('Kubik plugin changed during promotion; recovery files retained')
    observed_config = prepared_config.with_name('observed-config.yaml')
    config_moved = False
    try:
        staged.rename(destination)
        sync_directory(destination.parent)
        sync_directory(staged.parent)
        config_path.rename(observed_config)
        config_moved = True
        sync_directory(config_path.parent)
        sync_directory(observed_config.parent)
        if observed_config.is_symlink() or observed_config.read_bytes() != before:
            raise ValueError('config.yaml changed during installation; no settings were overwritten')
        os.link(prepared_config, config_path, follow_symlinks=False)
        sync_directory(config_path.parent)
    except BaseException:
        try:
            if config_moved:
                try:
                    os.link(observed_config, config_path, follow_symlinks=False)
                except FileExistsError:
                    pass
        finally:
            rollback_plugin(destination, staged, previous, state)
        raise


def promote_with_backup(staged, destination, config_path, prepared_config, before, previous, backup, expected=None):
    created = not backup.exists()
    if created:
        private_write(backup, before)
    else:
        backup.chmod(0o600)
    try:
        promote(staged, destination, config_path, prepared_config, before, previous, expected)
    except BaseException:
        observed_config = prepared_config.with_name('observed-config.yaml')
        try:
            original_restored = (observed_config.read_bytes() == before
                                 and os.path.samefile(observed_config, config_path))
        except (FileNotFoundError, OSError):
            original_restored = False
        if created and original_restored:
            backup.unlink(missing_ok=True)
        raise


def install(source, home, port=None, bind=None, check_only=False):
    home = home.expanduser().resolve()
    config_path = home / 'config.yaml'
    destination = home / 'plugins' / 'kubik'
    regular_destination(destination.parent, directory=True)
    regular_destination(destination, directory=True)
    backup = home / 'config.before-kubik.yaml'
    regular_destination(backup)
    if check_only:
        if (home / '.kubik-install.json').exists():
            raise ValueError('Interrupted Kubik installation: rerun this installer without --check to recover')
        _before, _after, chosen_port = configuration(config_path, port, bind)
        print(f'Configuration valid for Kubik / Hermes 0.22.2: {home}; port {chosen_port}. No changes made.')
        print('ffmpeg: ' + ('available' if shutil.which('ffmpeg') else 'missing; install it for speech playback'))
        return
    with installation_lock(home):
        recover_installation(home)
        before, after, chosen_port = configuration(config_path, port, bind)
        destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        with installation_work(home, destination) as (plugin_work, config_work, state):
            staged = plugin_work / 'plugin'
            staged.mkdir(mode=0o700)
            for path in sorted(source.iterdir()):
                if path.is_file() and path.suffix in ('.py', '.yaml', '.txt'):
                    private_write(staged / path.name, path.read_bytes())
            prepared_config = config_work / 'config.yaml'
            private_write(prepared_config, after)
            identity = staged.stat()
            state.update(phase='prepared', identity=[identity.st_dev, identity.st_ino])
            sync_directory(staged)
            sync_directory(plugin_work)
            sync_directory(config_work)
            sync_directory(destination.parent)
            write_transaction(home, state)
            promote_with_backup(staged, destination, config_path, prepared_config,
                                before, plugin_work / 'previous', backup, state)
            if not (os.path.samefile(prepared_config, config_path) and same_identity(destination, state['identity'])):
                raise ValueError('config.yaml changed during installation; recovery required')
            write_transaction(home, {**state, 'phase': 'committed'})
            recover_installation(home)
    print(f'Kubik installed in {home}. Restart the existing Hermes Gateway only when idle.')
    print(f'Device address: kubik://<reachable-Hermes-host>:{chosen_port}')
    print('Approve the code on Kubik: ' + shlex.join(
        [sys.executable, str(destination / 'pair.py'), 'CODE']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--home', type=Path, default=Path(os.environ.get('HERMES_HOME', Path.home() / '.hermes')))
    parser.add_argument('--port', type=int, help='TLS port; preserve the configured port when omitted')
    parser.add_argument('--bind', help='Listener IP; preserve the configured address when omitted')
    parser.add_argument('--check', action='store_true', help='Read-only runtime and configuration preflight')
    args = parser.parse_args()
    try:
        require_runtime()
        install(Path(__file__).resolve().parent.parent / 'hermes-kubik', args.home,
                args.port, args.bind, args.check)
    except (OSError, ValueError) as error:
        parser.exit(1, f'Kubik installation: {error}\n')


if __name__ == '__main__':
    main()
