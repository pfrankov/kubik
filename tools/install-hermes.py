#!/usr/bin/env python3
"""Install the Kubik platform into an existing Hermes home. Does not stop or start Hermes."""
import argparse
import os
from pathlib import Path
import shutil
import tempfile


def install(source, home, port):
    import yaml
    destination = home / 'plugins' / 'kubik'
    destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    config_path = home / 'config.yaml'
    if not config_path.is_file():
        raise ValueError('Set --home to an already configured Hermes installation')
    config = yaml.safe_load(config_path.read_text()) or {}
    plugins = config.setdefault('plugins', {})
    enabled = plugins.setdefault('enabled', [])
    if not isinstance(enabled, list):
        raise ValueError('Hermes plugins.enabled must be a list')
    if 'kubik-platform' not in enabled:
        enabled.append('kubik-platform')
    disabled = plugins.get('disabled', [])
    plugins['disabled'] = [name for name in disabled if name != 'kubik-platform']
    platform = config.setdefault('platforms', {}).setdefault('kubik', {})
    platform['enabled'] = True
    platform.setdefault('extra', {})['port'] = port
    config_text = yaml.safe_dump(config, sort_keys=False, allow_unicode=True)
    with tempfile.TemporaryDirectory(prefix='kubik-', dir=destination.parent) as temporary:
        staged = Path(temporary) / 'plugin'
        staged.mkdir()
        for path in sorted(source.iterdir()):
            if path.is_file() and path.suffix in ('.py', '.yaml'):
                shutil.copyfile(path, staged / path.name)
        old = home / 'kubik-plugin.previous'
        if old.exists():
            shutil.rmtree(old)
        if destination.exists():
            destination.rename(old)
        staged.rename(destination)
    backup = config_path.with_name('config.before-kubik.yaml')
    if not backup.exists():
        shutil.copyfile(config_path, backup)
        backup.chmod(0o600)
    temporary = config_path.with_suffix('.kubik.tmp')
    temporary.write_text(config_text)
    temporary.chmod(0o600)
    temporary.replace(config_path)
    print(f'Kubik installed inside Hermes. TLS port: {port}. Restart the gateway when idle.')
    print('Device Server address: kubik://<reachable-Hermes-address>:' + str(port))
    print('Approve its matching code: python ' + str(destination / 'pair.py') + ' CODE')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--home', type=Path, default=Path(os.environ.get('HERMES_HOME', str(Path.home() / '.hermes'))))
    parser.add_argument('--port', type=int, default=18793)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error('port must be 1024..65535')
    install(Path(__file__).resolve().parent.parent / 'hermes-kubik', args.home.expanduser(), args.port)


if __name__ == '__main__':
    main()
