#!/usr/bin/env python3
"""Install Kubik into an existing Hermes home; never restart services or change providers."""

import argparse
from collections.abc import MutableMapping
import contextlib
import fcntl
import importlib.util
import io
import ipaddress
import os
from pathlib import Path
import shutil
import shlex
import sys
import tempfile


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


@contextlib.contextmanager
def installation_lock(home):
    descriptor = os.open(home / '.kubik-install.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        os.fchmod(descriptor, 0o600)
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(descriptor)


def promote(staged, destination, config_path, prepared_config, before, previous):
    had_plugin = destination.exists()
    if had_plugin:
        destination.rename(previous)
    observed_config = prepared_config.with_name('observed-config.yaml')
    config_moved = False
    try:
        staged.rename(destination)
        config_path.rename(observed_config)
        config_moved = True
        if observed_config.is_symlink() or observed_config.read_bytes() != before:
            raise ValueError('config.yaml changed during installation; no settings were overwritten')
        os.link(prepared_config, config_path, follow_symlinks=False)
    except BaseException:
        try:
            if config_moved:
                try:
                    os.link(observed_config, config_path, follow_symlinks=False)
                except FileExistsError:
                    pass
        finally:
            if destination.exists():
                destination.rename(staged)
            if had_plugin:
                previous.rename(destination)
        raise


def promote_with_backup(staged, destination, config_path, prepared_config, before, previous, backup):
    created = not backup.exists()
    if created:
        private_write(backup, before)
    else:
        backup.chmod(0o600)
    try:
        promote(staged, destination, config_path, prepared_config, before, previous)
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
        _before, _after, chosen_port = configuration(config_path, port, bind)
        print(f'Configuration valid for Kubik / Hermes 0.22.2: {home}; port {chosen_port}. No changes made.')
        print('ffmpeg: ' + ('available' if shutil.which('ffmpeg') else 'missing; install it for speech playback'))
        return
    with installation_lock(home):
        before, after, chosen_port = configuration(config_path, port, bind)
        destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.kubik-plugin-', dir=destination.parent) as plugin_temporary, \
             tempfile.TemporaryDirectory(prefix='.kubik-config-', dir=home) as config_temporary:
            plugin_work, config_work = Path(plugin_temporary), Path(config_temporary)
            staged = plugin_work / 'plugin'
            staged.mkdir(mode=0o700)
            for path in sorted(source.iterdir()):
                if path.is_file() and path.suffix in ('.py', '.yaml', '.txt'):
                    shutil.copyfile(path, staged / path.name)
            prepared_config = config_work / 'config.yaml'
            private_write(prepared_config, after)
            promote_with_backup(staged, destination, config_path, prepared_config,
                                before, plugin_work / 'previous', backup)
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
