"""Read each character's authoritative partition CSV for build/package checks."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def partition_table(character):
    if character not in {'TESS', 'PLUSH'}:
        raise ValueError('Unknown firmware character')
    return ROOT / 'firmware' / ('partitions.csv' if character == 'TESS' else 'partitions-plush.csv')


def partitions(character):
    result = {}
    for line in partition_table(character).read_text().splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        fields = [value.strip() for value in line.split(',')]
        name, kind, subtype, offset, size = fields[:5]
        result[name] = {'type': kind, 'subtype': subtype, 'offset': int(offset, 0), 'size': int(size, 0)}
    return result
