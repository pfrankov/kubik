#!/usr/bin/env python3
"""Apply the required printer central role to warm IDF configs, preserving other choices."""
from pathlib import Path
import re
import sys

KEY = 'CONFIG_BT_NIMBLE_ROLE_CENTRAL'


def configure(output, defaults):
    required = re.findall(rf'^{KEY}=(.+)$', defaults.read_text(), re.M)
    if required != ['y']:
        raise ValueError('Printer defaults must enable the NimBLE central role')
    if not output.exists():
        return False  # Fresh IDF configuration reads the same defaults itself.
    source = output.read_text()
    pattern = rf'^(?:{KEY}=.*|# {KEY} is not set)\n?'
    current = re.findall(pattern, source, flags=re.M)
    if len(current) == 1 and current[0].strip() == f'{KEY}=y':
        return False
    lines = re.sub(pattern, '', source, flags=re.M)
    text = lines.rstrip('\n') + f'\n{KEY}=y\n'
    if source == text:
        return False
    output.write_text(text)
    return True


if __name__ == '__main__':
    configure(*(Path(value) for value in sys.argv[1:]))
