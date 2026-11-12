"""Turn-scoped snapshots of images returned by the trusted generation tool."""
import json
import os
from pathlib import Path
import secrets
import stat
import threading

from .printer_image_raster import MAX_INPUT_BYTES


class ImageOrigin:
    def __init__(self):
        self.lock = threading.Lock()
        self.images = {}
        self.calls = {}
        self.closed = False

    def close(self):
        with self.lock:
            self.closed = True
            self.images.clear()
            self.calls.clear()

    def get(self, ref):
        with self.lock:
            return None if self.closed else self.images.get(ref)

    def capture(self, home, call_id, result):
        if not isinstance(call_id, str) or not call_id or len(call_id) > 256:
            return None
        try:
            data = generated_image(home, result)
            if data is None:
                return None
        except (ValueError, TypeError, KeyError, OSError):
            return None
        with self.lock:
            if self.closed:
                return None
            if call_id in self.calls:
                ref = self.calls[call_id]
                return ref if self.images[ref] == data else None
            if len(self.images) >= 4 or sum(map(len, self.images.values())) + len(data) > MAX_INPUT_BYTES:
                return None
            ref = secrets.token_hex(16)
            self.images[ref], self.calls[call_id] = data, ref
            return ref


def generated_image(home, result):
    value = json.loads(result) if isinstance(result, str) else result
    if not isinstance(value, dict) or value.get('success') is not True or value.get('provider') != 'openai':
        return None
    path = Path(value['image'])
    directory = (Path(home) / 'cache/generated/images').resolve(strict=True)
    if not path.is_absolute() or path.parent.resolve(strict=True) != directory or path.suffix.lower() != '.png':
        return None
    return read_image(directory, path.name)


def read_image(directory, name):
    directory_fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory_fd)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or not 1 <= info.st_size <= MAX_INPUT_BYTES:
                raise ValueError('invalid_image')
            with os.fdopen(fd, 'rb', closefd=False) as image:
                data = image.read(MAX_INPUT_BYTES + 1)
            if len(data) != info.st_size:
                raise ValueError('image_changed')
            return data
        finally:
            os.close(fd)
    finally:
        os.close(directory_fd)
