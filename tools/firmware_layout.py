"""Read the character partitions and target layout used by release tooling."""
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parent.parent
TARGETS = {
    'esp32c6': {'chip_id': 13, 'board': 'waveshare-esp32c6-touch-amoled-2.16',
                'builds': {'TESS': 'build', 'PLUSH': 'build-plush'}},
    'esp32s3': {'chip_id': 9, 'board': 'waveshare-esp32s3-touch-amoled-1.8-v2',
                'builds': {'TESS': 'build-s3', 'PLUSH': 'build-s3-plush'}},
}


def require_character(character):
    if not isinstance(character, str) or character not in {'TESS', 'PLUSH'}:
        raise ValueError('Unknown firmware character')


def require_target(target):
    if not isinstance(target, str) or target not in TARGETS:
        raise ValueError(f'Unknown firmware target: {target}')


def partition_table(character):
    require_character(character)
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


def build_directory(character, target):
    require_character(character)
    require_target(target)
    return ROOT / 'firmware' / TARGETS[target]['builds'][character]


def firmware_filename(character, target):
    require_character(character)
    require_target(target)
    suffix = '' if target == 'esp32c6' else f'-{target}'
    return f'kubik-firmware-{character.lower()}{suffix}.zip'


def kit_filename(character, target):
    require_character(character)
    require_target(target)
    suffix = '' if target == 'esp32c6' else f'-{target}'
    return f'kubik-kit-{character.lower()}{suffix}.zip'


def target_chip_id(target):
    require_target(target)
    return TARGETS[target]['chip_id']


def target_board(target):
    require_target(target)
    return TARGETS[target]['board']


def build_target(build):
    """Require ESP-IDF's project description and generated config to agree."""
    description = json.loads((build / 'project_description.json').read_text())
    if not isinstance(description, dict):
        raise ValueError('Build project description is invalid')
    target = description.get('target')
    require_target(target)
    config = (build / 'config/sdkconfig.h').read_text()
    match = re.search(r'^#define CONFIG_IDF_TARGET "([^"]+)"$', config, re.MULTILINE)
    if not match or match.group(1) != target:
        raise ValueError('Build target metadata differs between project_description.json and generated sdkconfig.h')
    return target


def validate_image_chip(data, target, name):
    """Validate the ESP image header used by bootloader and app binaries."""
    expected = target_chip_id(target)
    if len(data) < 14 or data[0] != 0xE9:
        raise ValueError(f'{name} is not a valid ESP image')
    actual = int.from_bytes(data[12:14], 'little')
    if actual != expected:
        raise ValueError(f'{name} chip ID {actual} does not match {target} ({expected})')
