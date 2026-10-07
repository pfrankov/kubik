"""Shared isolated Hermes test setup; tests use local sockets and provider stubs."""
import asyncio
import base64
import os
from pathlib import Path
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parents[2]
pkg = ModuleType('hermes_kubik')
pkg.__path__ = [str(ROOT / 'hermes-kubik')]
sys.modules[pkg.__name__] = pkg
for name in ('gateway', 'gateway.config', 'gateway.platforms',
             'gateway.platforms.base', 'gateway.platforms.event'):
    sys.modules[name] = ModuleType(name)


class Base:
    def __init__(self, config, platform):
        self.config, self.platform = config, platform

    def build_source(self, chat_id, **kwargs):
        return SimpleNamespace(chat_id=chat_id, **kwargs)

    def _event_session_key(self, event):
        return event.source.chat_id

    async def cancel_session_processing(self, key, **kwargs):
        pass


sys.modules['gateway.config'].Platform = str
sys.modules['gateway.platforms.base'].BasePlatformAdapter = Base
sys.modules['gateway.platforms.base'].SendResult = lambda **kwargs: SimpleNamespace(**kwargs)
sys.modules['gateway.platforms.event'].MessageEvent = lambda **kwargs: SimpleNamespace(**kwargs)
sys.modules['gateway.platforms.event'].MessageType = SimpleNamespace(VOICE='voice')
sys.modules['tools'] = ModuleType('tools')
sys.modules['tools.transcription_tools'] = ModuleType('tools.transcription_tools')

from hermes_kubik.security import Security, atomic_json
from hermes_kubik.codec import decode, encode
from hermes_kubik.adapter import KubikAdapter, Peer


class DeviceSocket:
    """Controllable socket for protocol and connection-boundary tests."""
    def __init__(self, release_close=None):
        self.closed = False
        self.release_close = release_close
        self.prepared = asyncio.Event()
        self.close_started = asyncio.Event()
        self.finished = asyncio.Event()
        self.messages = []

    async def prepare(self, request):
        self.prepared.set()

    async def send_json(self, value):
        self.messages.append(value)

    async def send_bytes(self, value):
        self.messages.append(value)

    async def close(self, **kwargs):
        self.close_started.set()
        if self.release_close is not None:
            await self.release_close.wait()
        self.closed = True
        self.finished.set()

    def __aiter__(self):
        return self

    async def __anext__(self):
        await self.finished.wait()
        raise StopAsyncIteration


class ProtocolFixture(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        environment = patch.dict(os.environ, {'HERMES_HOME': self.temp.name})
        environment.start()
        self.addCleanup(environment.stop)
        self.store = Security(self.temp.name)
        self.store.tls()
        self.key = ec.generate_private_key(ec.SECP256R1())
        raw = self.key.public_key().public_bytes(serialization.Encoding.X962,
                                                serialization.PublicFormat.UncompressedPoint)
        self.hello = {'t': 'hello', 'v': 5, 'device': 'kubik-test', 'fw': 'test',
                      'key': base64.b64encode(raw).decode(), 'volume': 50}
