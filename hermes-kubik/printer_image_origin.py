"""Bounded connection-scoped snapshots returned by the trusted generation tool."""
import json
import os
from pathlib import Path
import secrets
import stat
import threading
import time

from .printer_image_raster import MAX_INPUT_BYTES


class ImageOrigin:
    def __init__(self, *, now=time.monotonic, loop=None, ttl=600):
        self.lock = threading.Lock()
        self.now, self.loop, self.ttl = now, loop, ttl
        self.timer = None
        self.uncertain = set()
        self.images = {}
        self.calls = {}
        self.closed = False

    def close(self):
        with self.lock:
            self.closed = True
            self.images.clear()
            self.calls.clear()
            self.uncertain.clear()
        self.schedule()

    def prune(self):
        expired = [ref for ref, (stamp, _) in self.images.items() if self.now() - stamp >= self.ttl]
        for ref in expired:
            self.remove(ref)

    def remove(self, ref):
        self.images.pop(ref, None)
        self.uncertain.discard(ref)
        self.calls = {call: saved for call, saved in self.calls.items() if saved != ref}

    def schedule(self):
        if self.loop is not None:
            try:
                self.loop.call_soon_threadsafe(self.arm)
            except RuntimeError:
                pass

    def arm(self):
        with self.lock:
            if self.timer:
                self.timer.cancel()
                self.timer = None
            if self.closed or not self.images:
                return
            deadline = min(stamp for stamp, _ in self.images.values()) + self.ttl
            self.timer = self.loop.call_later(max(0, deadline - self.now()), self.expire)

    def expire(self):
        with self.lock:
            self.prune()
        self.arm()

    def mark_uncertain(self, ref):
        with self.lock:
            if ref in self.images:
                self.uncertain.add(ref)

    def is_uncertain(self, ref):
        with self.lock:
            self.prune()
            return ref in self.uncertain

    def get(self, ref):
        with self.lock:
            self.prune()
            item = None if self.closed else self.images.get(ref)
            return item[1] if item else None

    def contains(self, data):
        with self.lock:
            self.prune()
            return not self.closed and any(item[1] == data for item in self.images.values())

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
            self.prune()
            if call_id in self.calls:
                ref = self.calls[call_id]
                return ref if self.images[ref][1] == data else None
            while self.images and (len(self.images) >= 4 or sum(len(item[1]) for item in self.images.values()) + len(data) > MAX_INPUT_BYTES):
                self.remove(next(iter(self.images)))
            ref = secrets.token_hex(16)
            self.images[ref], self.calls[call_id] = (self.now(), data), ref
        self.schedule()
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
