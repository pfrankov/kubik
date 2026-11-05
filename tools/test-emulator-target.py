#!/usr/bin/env python3
"""Exercise rectangular native frames and input bounds without USB or a browser."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('kubik_emulator', ROOT / 'tools/emulator.py')
emulator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(emulator)


def decoded_png(data):
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    offset, payload, size = 8, bytearray(), None
    while offset < len(data):
        length, = struct.unpack_from('>I', data, offset)
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        if kind == b'IHDR': size = struct.unpack('>IIBBBBB', body)
        if kind == b'IDAT': payload.extend(body)
        offset += length + 12
    assert size is not None and size[2:] == (8, 2, 0, 0, 0)
    raw = zlib.decompress(payload)
    assert len(raw) == (size[0] * 3 + 1) * size[1]
    return size[:2], raw


def invalid_events(worker):
    for command, args in (
        ('touches', [1, 480, 50, 1, 0, 0, 0, 0]),
        ('touches', [1, 40, 584, 1, 0, 0, 0, 0]),
        ('touches', [1, 40, 592, 1, 0, 0, 0, 0]),
        ('touches', [1, 40, 50, 1, 1, 90, 100, 2]),
        ('event', [0, 40, 584]),
    ):
        try: worker.event({'command': command, 'args': args})
        except ValueError: pass
        else: raise AssertionError(f'Invalid S3 event accepted: {command} {args}')


def check_target(output=None):
    worker = emulator.Worker(emulator.build('esp32s3'))
    samples = 0
    try:
        assert worker.metadata == {'target': 'esp32s3', 'width': 368, 'height': 448,
                                   'render_width': 384, 'render_height': 448, 'input_width': 480, 'input_height': 584, 'touch_capacity': 1, 'key_button': False}
        if output: output.mkdir(parents=True, exist_ok=True)
        for character in (1, 0):
            worker.event({'command': 'character', 'args': [character]})
            for index, name in enumerate(worker.screens):
                worker.event({'command': 'select', 'args': [index]})
                state, image = worker.frame(28)
                assert state['name'] == name
                size, _ = decoded_png(image)
                assert size == (368, 448)
                if output: (output / f'{character}-{index:02}-{name.replace(":", "-").replace(" ", "-")}.png').write_bytes(image)
                samples += 1
        worker.event({'command': 'select', 'args': [worker.screens.index('Play: skies')]})
        worker.frame(60)
        worker.event({'command': 'touches', 'args': [1, 479, 583, 15, 0, 0, 0, 0]})
        assert worker.frame()[0]['contacts'] == 1
        worker.event({'command': 'touches', 'args': [0, 0, 0, 0, 0, 0, 0, 0]})
        assert worker.frame()[0]['contacts'] == 0
        invalid_events(worker)
        # A native request below visible height must not become a phantom contact.
        worker.send('touches 1 20 584 1 0 0 0 0')
        worker.send('frame 1')
        assert worker.read_frame()[0]['contacts'] == 0
    finally:
        worker.close()
    print(f'S3 emulator: {samples} real screens/characters at native 368x448, 384-column padding hidden, full-height input and one-contact hardware boundary passed')


def check_png_boundaries():
    for width, height in ((480, 480), (368, 448)):
        rgb = bytes((index * 17) % 256 for index in range(width * height * 3))
        size, raw = decoded_png(emulator.png(rgb, width, height))
        assert size == (width, height)
        assert hashlib.sha256(b''.join(raw[y * (width * 3 + 1) + 1:(y + 1) * (width * 3 + 1)] for y in range(height))).digest() == hashlib.sha256(rgb).digest()
        try: emulator.png(rgb + b'padding', width, height)
        except ValueError: pass
        else: raise AssertionError('Unexpected trailing render padding entered the PNG')
    try: emulator.build('esp32')
    except ValueError: pass
    else: raise AssertionError('Unsupported target accepted')


def check_bad_metadata():
    with tempfile.TemporaryDirectory(prefix='kubik-emulator-metadata-') as temporary:
        fixture = Path(temporary) / 'broken-native'
        fixture.write_text('#!/usr/bin/env python3\nimport sys,json\nfor line in sys.stdin:\n if line.strip()=="metadata":\n  print(json.dumps({"target":"esp32s3","width":368,"height":448,"render_width":368,"render_height":400,"input_width":480,"input_height":584,"touch_capacity":1,"key_button":False}),flush=True)\n')
        fixture.chmod(0o755)
        try: emulator.Worker(fixture)
        except RuntimeError as error: assert 'metadata' in str(error)
        else: raise AssertionError('A clipped native framebuffer was accepted')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    options = parser.parse_args()
    check_png_boundaries()
    check_bad_metadata()
    check_target(options.out)
