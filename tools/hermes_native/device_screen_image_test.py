"""Safety invariants of the physical image attachment fixture."""
import asyncio
import hashlib
from pathlib import Path
import tempfile
import unittest
from io import BytesIO

from PIL import Image
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch

from device_screen_image import Observations, source_bytes, fixture_png


class AttachmentFixtureSafety(unittest.IsolatedAsyncioTestCase):
    def make(self):
        adapter = SimpleNamespace(command=AsyncMock(), authenticate=AsyncMock())
        observation = Observations(adapter)
        socket = SimpleNamespace(send_json=AsyncMock(), send_bytes=AsyncMock())
        observation.watch_socket(socket)
        return observation, socket

    async def test_baseline_waits_for_background_printer_and_checks_live_state(self):
        from device_screen_image import baseline_when_printer_idle
        states = [{'printer_busy': True}, {'printer_busy': False, 'live_active': True,
                  'mic_open': True, 'mic_rx_enabled': True}]

        async def until(predicate, label, timeout):
            self.assertEqual((label, timeout), ('printer background work finished', 40))
            self.assertFalse(predicate({}))
            self.assertFalse(predicate(states[0]))
            self.assertTrue(predicate(states[1]))
            return states[1]

        control = SimpleNamespace(until=until)
        self.assertIs(await baseline_when_printer_idle(control, True), states[1])
        states[1]['mic_open'] = False
        with self.assertRaises(AssertionError):
            await baseline_when_printer_idle(control, True)

    async def test_busy_timeout_is_not_hidden(self):
        from device_screen_image import baseline_when_printer_idle
        control = SimpleNamespace(until=AsyncMock(side_effect=RuntimeError('Timed out waiting for printer')))
        with self.assertRaisesRegex(RuntimeError, 'Timed out waiting for printer'):
            await baseline_when_printer_idle(control, False)

    async def test_live_monitor_starts_only_after_printer_is_idle(self):
        from device_screen_image import check_live
        events = []
        state = {'printer_busy': False, 'live_active': True, 'mic_open': True, 'mic_rx_enabled': True}

        async def until(predicate, label, timeout):
            events.append('idle')
            self.assertTrue(predicate(state))
            return state

        async def monitor(control, observations, stopped):
            events.append('monitor')
            await stopped.wait()

        async def check(*args, **kwargs):
            self.assertIs(kwargs['baseline'], state)
            await asyncio.sleep(0)
            events.append('check')
            return {}

        control = SimpleNamespace(request=AsyncMock(return_value={}), info=AsyncMock(return_value={}),
                                  sim=AsyncMock(), until=until, evidence=Path('/tmp'))
        peer = SimpleNamespace(volume=20, native=SimpleNamespace(current=lambda: True))
        observations = SimpleNamespace(persist=lambda *args: None, errors=[], connection=None)
        providers = SimpleNamespace(voice=None)
        with patch('device_voice.begin', AsyncMock(return_value=object())), \
             patch('device_screen_image.sample_live', monitor), patch('device_screen_image.check', check), \
             patch('voice_fixture.eventually', AsyncMock()), patch('device_screen_image.verify_live', return_value={}), \
             patch('device_screen_image.fault_window'):
            await check_live(None, peer, control, providers, observations)
        self.assertEqual(events, ['idle', 'monitor', 'check'])
        self.assertEqual(peer.volume, 20)

    async def test_every_printer_route_is_blocked_before_dispatch(self):
        observation, socket = self.make()
        for value in ({'t': 'printer', 'action': 'status'}, {'t': 'printer', 'action': 'print'},
                      {'t': 'printer_image', 'action': 'start'}):
            with self.assertRaisesRegex(AssertionError, 'printer'):
                await socket.send_json(value)
        with self.assertRaisesRegex(AssertionError, 'printer'):
            await socket.send_bytes(b'\x06\x00')
        observation.restore()
        self.assertEqual(socket.send_json.await_count, 0)
        self.assertEqual(socket.send_bytes.await_count, 0)

    async def test_one_complete_image_must_precede_one_show(self):
        observation, socket = self.make()
        rid = b'a' * 32
        with self.assertRaisesRegex(AssertionError, 'Unexpected'):
            await socket.send_json({'t': 'screen_image', 'action': 'show', 'rid': rid.decode()})
        for offset in range(0, 8224, 896):
            size = min(896, 8224 - offset)
            await socket.send_bytes(b'\x07\x01' + rid + offset.to_bytes(2, 'big') + bytes(size))
        await socket.send_json({'t': 'screen_image', 'action': 'show', 'rid': rid.decode()})
        self.assertEqual((observation.frames, observation.bytes, observation.shows), (10, 8224, 1))
        with self.assertRaisesRegex(AssertionError, 'Unexpected'):
            await socket.send_json({'t': 'screen_image', 'action': 'show', 'rid': rid.decode()})
        with self.assertRaisesRegex(AssertionError, 'Unexpected'):
            await socket.send_bytes(b'\x07\x01' + rid + b'\0\0x')
        observation.restore()

    async def test_reconnect_keeps_the_one_show_budget(self):
        observation, socket = self.make()
        peer = SimpleNamespace()
        observation.authenticate.return_value = peer
        observation.install()
        replacement = SimpleNamespace(send_json=AsyncMock(), send_bytes=AsyncMock())
        await observation.adapter.authenticate(replacement)
        self.assertEqual(len(observation.senders), 2)
        await socket.send_bytes(b'\x07\x01' + b'a' * 32 + b'\0\0x')
        with self.assertRaisesRegex(AssertionError, 'identity'):
            await replacement.send_bytes(b'\x07\x01' + b'b' * 32 + b'\0\1x')
        observation.restore()

    def test_verified_source_requires_exact_owner_digest_and_safe_png(self):
        peer = SimpleNamespace(identity='kubik-physical:key')
        image = BytesIO(); Image.new('RGB', (8, 8), 'blue').save(image, format='PNG')
        data = image.getvalue(); digest = hashlib.sha256(data).hexdigest()
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'source.png'; source.write_bytes(data)
            self.assertEqual(source_bytes(source, digest, peer.identity, peer), data)
            copy = Path(temp) / 'copy.png'
            evidence = fixture_png(copy, (source, digest, peer.identity), peer)
            self.assertEqual(copy.read_bytes(), data)
            self.assertEqual((evidence['source_sha256'], evidence['source_size']), (digest, len(data)))
            with self.assertRaisesRegex(ValueError, 'owner'):
                source_bytes(source, digest, 'other', peer)
            with self.assertRaisesRegex(ValueError, 'changed'):
                source_bytes(source, '0' * 64, peer.identity, peer)
            link = Path(temp) / 'link.png'; link.symlink_to(source)
            with self.assertRaises(OSError):
                source_bytes(link, digest, peer.identity, peer)
            source.write_bytes(b'not a PNG')
            with self.assertRaises(Exception):
                source_bytes(source, hashlib.sha256(b'not a PNG').hexdigest(), peer.identity, peer)

    def test_synthetic_image_is_default_without_source(self):
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / 'synthetic.png'
            evidence = fixture_png(target, None, SimpleNamespace(identity='any'))
            self.assertEqual(evidence, {'image': 'synthetic local fixture'})
            self.assertTrue(target.is_file())

    def test_live_verification_requires_presented_image_and_overlapping_microphone(self):
        from device_screen_image import verify_live
        state = {'heap_failures': 0, 'heap_internal': 10000, 'heap_dma': 9000,
                 'printer_busy': False, 'printer_preview_active': True, 'printer_preview_presented': True,
                 'live_active': True, 'mic_open': True, 'mic_rx_enabled': True, 'online': True}
        observed = SimpleNamespace(states=[{'at': 1, **state}, {'at': 3, **state}],
                                   errors=[], connection=SimpleNamespace(mic_times=[.5, 2, 4]))
        self.assertEqual(verify_live(observed, {'heap_failures': 0})['mic_frames_during_preview'], 1)
        observed.connection.mic_times = [.5, 4]
        with self.assertRaisesRegex(AssertionError, 'No microphone'):
            verify_live(observed, {'heap_failures': 0})
        observed.connection.mic_times = [2]
        observed.states[0]['printer_preview_presented'] = False
        with self.assertRaisesRegex(AssertionError, 'No microphone'):
            verify_live(observed, {'heap_failures': 0})
        observed.states[0]['printer_preview_presented'] = True
        observed.states[1]['heap_failures'] = 1
        with self.assertRaisesRegex(AssertionError, 'Heap allocation'):
            verify_live(observed, {'heap_failures': 0})
        observed.states[1]['heap_failures'] = 0
        observed.states[0]['printer_busy'] = True
        with self.assertRaisesRegex(AssertionError, 'Live microphone or connection stopped'):
            verify_live(observed, {'heap_failures': 0})
        observed.states[0]['printer_busy'] = False
        observed.states.append({'at': 2, **state, 'mic_open': False})
        with self.assertRaisesRegex(AssertionError, 'Live microphone or connection stopped'):
            verify_live(observed, {'heap_failures': 0})

    def test_mode_is_explicit_and_mutually_exclusive(self):
        from device import acceptance, arguments, long_mode
        argv = ['device.py', '--source', '/tmp/hermes', '--tls-identity', '/tmp/tls.json',
                '--control', 'http://127.0.0.1:18792', '--evidence', '/tmp/new-evidence']
        with patch('sys.argv', argv + ['--screen-image-check']):
            options = arguments()
        self.assertTrue(long_mode(options))
        self.assertEqual(acceptance(options)[0], Observations)
        with patch('sys.argv', argv + ['--screen-image-check', '--screen-image-source', '/tmp/a.png']):
            with self.assertRaises(SystemExit):
                arguments()
        with patch('sys.argv', argv + ['--screen-image-source', '/tmp/a.png', '--screen-image-source-sha256', 'a' * 64,
                                        '--screen-image-source-owner', 'device']):
            with self.assertRaises(SystemExit):
                arguments()
        with patch('sys.argv', argv + ['--screen-image-check', '--screen-image-source', '/tmp/a.png',
                                        '--screen-image-source-sha256', 'a' * 64, '--screen-image-source-owner', 'device']):
            options = arguments()
        self.assertEqual((str(options.screen_image_source), options.screen_image_source_owner), ('/tmp/a.png', 'device'))
        with patch('sys.argv', argv + ['--screen-image-live-check']):
            live = arguments()
        self.assertTrue(long_mode(live))
        self.assertEqual(acceptance(live)[0].__name__, 'LiveObservations')
        with patch('sys.argv', argv + ['--screen-image-live-check', '--screen-image-source', '/tmp/a.png',
                                        '--screen-image-source-sha256', 'a' * 64, '--screen-image-source-owner', 'device']):
            with self.assertRaises(SystemExit):
                arguments()
        with patch('sys.argv', argv + ['--screen-image-check', '--printer-image-check']):
            with self.assertRaises(SystemExit):
                arguments()
