#!/usr/bin/env python3
"""Initialize an independent Plush IDF config, retaining all non-partition options."""
from pathlib import Path
import re
import sys


def configure(source, defaults, output):
    # Existing build-local menuconfig is authoritative after initial migration.
    seed = output if output.exists() else source if source.exists() else defaults
    text = seed.read_text()
    for key in ('CONFIG_PARTITION_TABLE_CUSTOM_FILENAME', 'CONFIG_PARTITION_TABLE_FILENAME'):
        value = f'{key}="partitions-plush.csv"'
        pattern = rf'^{key}=.*$'
        text = re.sub(pattern, value, text, flags=re.M) if re.search(pattern, text, re.M) else text.rstrip() + '\n' + value + '\n'
    if not output.exists() or output.read_text() != text:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text)


if __name__ == '__main__':
    configure(*(Path(value) for value in sys.argv[1:]))
