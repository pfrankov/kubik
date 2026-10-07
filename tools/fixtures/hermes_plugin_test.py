"""Native protocol evidence independent of provider calls; Hermes API imports are checked on its server."""
import asyncio
import base64
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch

from aiohttp import ClientSession
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.exceptions import InvalidSignature

ROOT = Path(__file__).resolve().parents[2]
pkg = ModuleType('hermes_kubik')
pkg.__path__ = [str(ROOT / 'hermes-kubik')]
sys.modules[pkg.__name__] = pkg
for name in ('gateway', 'gateway.config', 'gateway.platforms', 'gateway.platforms.base', 'gateway.platforms.event'):
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
from hermes_kubik.security import Security, atomic_json
from hermes_kubik.codec import decode, encode
from hermes_kubik.adapter import KubikAdapter, Peer
sys.modules['tools'] = ModuleType('tools')
sys.modules['tools.transcription_tools'] = ModuleType('tools.transcription_tools')


class DeviceSocket:
    """A controllable close handshake for overlapping authenticated connections."""
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


class Protocol(unittest.IsolatedAsyncioTestCase):
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

    def sign(self, nonce, bind=None):
        text = '\n'.join(('kubik-auth-v5', nonce, self.hello['device'], self.hello['key'], bind or self.store.bind))
        return base64.b64encode(self.key.sign(text.encode(), ec.ECDSA(hashes.SHA256()))).decode()

    def test_identity_binding_and_pairing(self):
        identity = self.store.verify(self.hello, 'nonce', self.sign('nonce'))
        self.assertFalse(self.store.approved(identity))
        with self.assertRaises(InvalidSignature):
            self.store.verify(self.hello, 'other', self.sign('nonce'))
        with self.assertRaises(InvalidSignature):
            self.store.verify(self.hello, 'nonce', self.sign('nonce', 'relay'))
        code = self.store.pair(identity)
        self.assertEqual(code, self.store.pair(identity))
        self.store.pair('second'); self.store.pair('third')
        with self.assertRaises(ValueError):
            self.store.pair('fourth')
        atomic_json(self.store.allow_path, [identity])
        self.assertTrue(self.store.approved(identity))
        self.assertEqual(os.stat(self.store.allow_path).st_mode & 0o777, 0o600)
        wrong = {**self.hello, 'device': 'different'}
        other, _ = self.store.identity(wrong)
        self.assertFalse(self.store.approved(other))

    async def test_real_tls_welcome_and_receipt(self):
        adapter = KubikAdapter(SimpleNamespace(extra={'port': 19792, 'bind': '127.0.0.1'}, enabled=True))
        adapter.security = self.store
        adapter.speech_available = lambda: ({'available': True}, {'available': False})
        identity, _ = self.store.identity(self.hello)
        atomic_json(self.store.allow_path, [identity])
        await adapter.connect(is_reconnect=False)
        try:
            async with ClientSession() as session:
                async with session.ws_connect('https://127.0.0.1:19792/kubik/v1', ssl=False) as socket:
                    await socket.send_json(self.hello)
                    challenge = await asyncio.wait_for(socket.receive_json(), 5)
                    await socket.send_json({'t': 'auth', 'sig': self.sign(challenge['nonce'])})
                    self.assertEqual((await asyncio.wait_for(socket.receive_json(), 5))['t'], 'welcome')
                    self.assertEqual((await asyncio.wait_for(socket.receive_json(), 5))['voice_mode'], 'classic')
                    task = asyncio.create_task(adapter.send(chat_id=identity, content='Hello', metadata={}))
                    text = await asyncio.wait_for(socket.receive_json(), 5)
                    self.assertEqual(text['text'], 'Hello')
                    await socket.send_json({'t': 'shown', 'receipt': text['receipt']})
                    self.assertTrue((await task).success)
                    await socket.send_json({'t': 'ptt', 'on': True, 'turn': 5})
                    await socket.send_bytes(bytes((4, 6, 0, 0, 0, 0)))
                    closed = await asyncio.wait_for(socket.receive(), 5)
                    self.assertEqual(closed.data, 4002)
        finally:
            await adapter.disconnect()

    async def test_media_survives_background_dispatch(self):
        adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
        adapter.security = self.store
        identity, _ = self.store.identity(self.hello)
        atomic_json(self.store.allow_path, [identity])
        class Socket:
            closed = False
            async def send_json(self, value):
                pass
        peer = Peer(Socket(), identity, self.hello)
        adapter.peers[identity] = peer
        paths = []
        def native_stt(path, source):
            paths.append(path)
            self.assertTrue(Path(path).is_file())
            return {'success': True, 'transcript': 'Hello'}
        sys.modules['tools.transcription_tools'].transcribe_audio = native_stt
        async def native_handle(event):
            self.assertEqual(event.text, 'Hello')
            self.assertTrue(event.source.role_authorized)
            event._gateway_accepted = True
            async def background():
                await asyncio.sleep(.05)
                await adapter.on_processing_complete(event, 'success')
            asyncio.create_task(background())
        adapter.handle_message = native_handle
        await adapter.dispatch(peer, bytes(1920))
        self.assertFalse(Path(paths[0]).exists())

    async def test_reconnect_replaces_a_peer_before_awaiting_its_close(self):
        adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
        adapter.speech_available = lambda: ({'available': True}, {'available': False})
        identity, _ = self.store.identity(self.hello)
        release_old = asyncio.Event()
        old, first, latest = DeviceSocket(release_old), DeviceSocket(), DeviceSocket()
        adapter.peers[identity] = Peer(old, identity, self.hello)

        async def authenticate(socket):
            return Peer(socket, identity, self.hello)

        adapter.authenticate = authenticate
        tasks = []
        with patch('hermes_kubik.adapter.web.WebSocketResponse', side_effect=[first, latest]):
            try:
                tasks.append(asyncio.create_task(adapter.accept(None)))
                await asyncio.wait_for(old.close_started.wait(), 1)
                tasks.append(asyncio.create_task(adapter.accept(None)))
                await asyncio.wait_for(latest.prepared.wait(), 1)
                self.assertTrue(first.closed, 'the second reconnect must close its actual predecessor')
                release_old.set()
                await asyncio.wait_for(tasks[0], 1)
                self.assertIs(adapter.peers[identity].socket, latest)
                self.assertEqual(first.messages, [], 'a superseded handshake must not announce readiness')
                self.assertEqual(latest.messages[0]['t'], 'welcome')
            finally:
                release_old.set()
                await first.close()
                await latest.close()
                await asyncio.gather(*tasks, return_exceptions=True)
        self.assertFalse(adapter.peers)
        self.assertFalse(adapter.sockets)

    async def test_queued_speech_rechecks_mute_and_connection_before_playback(self):
        from hermes_kubik import speech
        for change in ('interrupt', 'volume', 'disconnect'):
            with self.subTest(change=change):
                adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
                peer = Peer(DeviceSocket(), 'device', self.hello)
                peer.stage = 'agent'
                adapter.peers['device'] = peer
                started, release = asyncio.Event(), asyncio.Event()
                calls = []

                async def play_file(current, path):
                    calls.append(path)
                    started.set()
                    await release.wait()
                    return SimpleNamespace(success=True)

                tasks = []
                with patch.object(speech, 'play_file', play_file):
                    try:
                        tasks.append(asyncio.create_task(adapter.send_voice('device', 'first.wav')))
                        await asyncio.wait_for(started.wait(), 1)
                        tasks.append(asyncio.create_task(adapter.send_voice('device', 'queued.wav')))
                        await asyncio.sleep(0)  # the second send is waiting on the output lock
                        if change == 'interrupt':
                            await adapter.cancel_request(peer, {'gen': peer.gen})
                        elif change == 'volume':
                            peer.volume = 0
                        else:
                            await peer.socket.close()
                        release.set()
                        results = await asyncio.wait_for(asyncio.gather(*tasks), 1)
                        self.assertEqual(calls, ['first.wav'])
                        self.assertEqual(results[1].success, change != 'disconnect')
                    finally:
                        release.set()
                        await asyncio.gather(*tasks, return_exceptions=True)

    async def test_partial_playback_and_cancel_preserve_agent_run(self):
        adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
        class Socket:
            closed = False
            async def send_json(self, value):
                pass
        identity, _ = self.store.identity(self.hello)
        peer = Peer(Socket(), identity, self.hello)
        peer.gen, peer.sent_ms = 8, 1000
        receipt = asyncio.Event()
        peer.receipts[('played', 8)] = receipt
        await adapter.command(peer, {'t': 'played', 'gen': 8, 'ms': 500})
        self.assertFalse(receipt.is_set())
        await adapter.command(peer, {'t': 'played', 'gen': 8, 'ms': 1000})
        self.assertTrue(receipt.is_set())
        peer.stage = 'agent'
        peer.task = asyncio.create_task(asyncio.sleep(.05))
        await adapter.cancel_request(peer, {'gen': 8})
        self.assertTrue(peer.muted)
        self.assertFalse(peer.task.cancelled())
        previous = peer.task
        await adapter.ptt(peer, {'turn': 9, 'on': True})
        self.assertEqual(peer.turn, 9)
        self.assertTrue(peer.muted)
        await adapter.ptt(peer, {'turn': 99, 'on': True})
        self.assertEqual(peer.turn, 9)
        receipt.clear()
        await adapter.cancel_request(peer, {'turn': 9})
        self.assertIsNone(peer.turn)
        self.assertFalse(receipt.is_set())
        self.assertFalse(previous.cancelled())
        await adapter.ptt(peer, {'turn': 10, 'on': True})
        peer.pcm.extend(bytes(48))
        calls = []
        async def dispatch_after_previous(peer, pcm):
            self.assertTrue(previous.done())
            calls.append(pcm)
        adapter.dispatch = dispatch_after_previous
        await adapter.ptt(peer, {'turn': 10, 'on': False})
        await peer.task
        self.assertEqual(calls, [bytes(48)])
        self.assertFalse(previous.cancelled())
        peer.stage, peer.muted = 'agent', True
        previous = peer.task = asyncio.create_task(asyncio.sleep(.05))
        await adapter.ptt(peer, {'turn': 11, 'on': True})
        peer.pcm.extend(bytes(48))
        await adapter.ptt(peer, {'turn': 11, 'on': False})
        await adapter.ptt(peer, {'turn': 99, 'on': True})
        self.assertEqual(peer.input_turn, 11)
        await adapter.cancel_request(peer, {'turn': 11})
        self.assertIs(peer.task, previous)
        self.assertFalse(previous.cancelled())
        await previous
        self.assertEqual(len(calls), 1)  # cancelled queued input is never dispatched
        receipt.clear()
        peer.task = None
        await adapter.cancel(peer)
        self.assertFalse(receipt.is_set())

    def test_installer_preserves_existing_configuration(self):
        import yaml
        spec = importlib.util.spec_from_file_location('installer', ROOT / 'tools/install-hermes.py')
        installer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(installer)
        home = Path(self.temp.name)
        config = {'model': 'configured-model', 'platforms': {'telegram': {'enabled': True}},
                  'plugins': {'enabled': ['existing-plugin']}}
        (home / 'config.yaml').write_text(yaml.safe_dump(config))
        installer.install(ROOT / 'hermes-kubik', home, 18793)
        saved = yaml.safe_load((home / 'config.yaml').read_text())
        self.assertEqual(saved['model'], config['model'])
        self.assertEqual(saved['platforms']['telegram'], config['platforms']['telegram'])
        self.assertEqual(saved['plugins']['enabled'], ['existing-plugin', 'kubik-platform'])
        self.assertTrue((home / 'plugins/kubik/plugin.yaml').exists())
        installer.install(ROOT / 'hermes-kubik', home, 18793)
        manifests = list((home / 'plugins').rglob('plugin.yaml'))
        self.assertEqual(manifests, [home / 'plugins/kubik/plugin.yaml'])
        self.assertTrue((home / 'kubik-plugin.previous/plugin.yaml').exists())

    def test_volume_boundary(self):
        for volume in ('30', {}, True, -1, 101):
            with self.assertRaises(ValueError):
                Peer(None, 'device', {**self.hello, 'volume': volume})
        for volume in (0, 20, 100):
            self.assertEqual(Peer(None, 'device', {**self.hello, 'volume': volume}).volume, volume)

    async def test_speech_generation_wraps_without_using_reserved_zero(self):
        from hermes_kubik.speech import send_pcm
        adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
        socket = DeviceSocket()
        peer = Peer(socket, 'device', self.hello)
        peer.gen = 254
        send_json = socket.send_json

        async def acknowledge(value):
            await send_json(value)
            if value['t'] == 'speak_end':
                await adapter.command(peer, {'t': 'played', 'gen': value['gen'], 'ms': 40})

        socket.send_json = acknowledge
        for _ in range(2):
            self.assertTrue((await send_pcm(peer, bytes(1920))).success)
        starts = [value['gen'] for value in socket.messages if isinstance(value, dict) and value['t'] == 'speak']
        self.assertEqual(starts, [255, 1])
        self.assertEqual([value[:2] for value in socket.messages if isinstance(value, bytes)],
                         [bytes((3, 255)), bytes((3, 1))])

    def test_ima_shared_vector_and_bounds(self):
        vector = bytes.fromhex('0000007777ffff0000')
        samples = struct.unpack('<12h', decode(vector))
        self.assertEqual(samples, (11, 41, 104, 240, -53, -684, -2041, -4951, -4536, -4158, -3815, -3503))
        pcm = struct.pack('<4h', 0, 100, -100, 0)
        encoded = encode(pcm, [0, 0])
        self.assertEqual(len(decode(encoded)), len(pcm))
        for invalid in (bytes((0, 0, 89, 0)), bytes(484), bytes(3)):
            with self.assertRaises(ValueError):
                decode(invalid)


unittest.main()
