#!/usr/bin/env python3
"""Apply the shared BLE role/security policy to warm configs, preserving other choices."""
from pathlib import Path
import re
import sys

REQUIRED = {'CONFIG_BT_NIMBLE_ROLE_CENTRAL': 'y', 'CONFIG_BT_NIMBLE_SECURITY_ENABLE': 'n'}


def apply_value(source, key, value):
    pattern = rf'^(?:{key}=.*|# {key} is not set)\n?'
    current = re.findall(pattern, source, flags=re.M)
    canonical = f'{key}=y' if value == 'y' else f'# {key} is not set'
    if len(current) == 1 and current[0].strip() == canonical:
        return source
    lines = re.sub(pattern, '', source, flags=re.M)
    return lines.rstrip('\n') + '\n' + canonical + '\n'


def configure(output, defaults):
    text = defaults.read_text()
    for key, value in REQUIRED.items():
        if re.findall(rf'^{key}=(.+)$', text, re.M) != [value]:
            raise ValueError('Shared BLE defaults must enable central and disable unused SMP')
    if not output.exists():
        return False  # Fresh IDF configuration reads the same defaults itself.
    source = output.read_text()
    text = source
    for key, value in REQUIRED.items():
        text = apply_value(text, key, value)
    if source == text:
        return False
    output.write_text(text)
    return True


if __name__ == '__main__':
    configure(*(Path(value) for value in sys.argv[1:]))
