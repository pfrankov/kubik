#!/usr/bin/env python3
"""Install CI packages with bounded mirror retries and visible, fatal errors."""
import re
import shlex
import subprocess
import sys

# CLI options override image apt.conf fragments. Hosted Ubuntu mirrors can stall
# otherwise: https://github.com/actions/runner-images/issues/14594
APT = ['sudo', 'timeout', '--kill-after=5s', '180s', 'apt-get',
       '-o', 'Acquire::Retries=1', '-o', 'Acquire::http::Timeout=15',
       '-o', 'Acquire::https::Timeout=15', '-o', 'DPkg::Lock::Timeout=30']


def install(packages):
    if not packages or any(not re.fullmatch(r'[a-z0-9][a-z0-9+.-]*', name) for name in packages):
        raise ValueError('Supply explicit Debian package names')
    for arguments in (['update', '--error-on=any'], ['install', '-y', *packages]):
        command = [*APT, *arguments]
        print(shlex.join(command), flush=True)
        subprocess.run(command, check=True)


if __name__ == '__main__':
    install(sys.argv[1:])
