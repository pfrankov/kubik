"""Physical image fixture refuses unintended and repeated printer jobs."""
import unittest
from types import SimpleNamespace
from unittest.mock import AsyncMock

from device_image import Observations


class ImageFixtureSafety(unittest.IsolatedAsyncioTestCase):
    async def test_one_image_start_and_no_other_prints(self):
        socket = SimpleNamespace(send_json=AsyncMock(), send_bytes=AsyncMock())
        observation = Observations(SimpleNamespace(command=AsyncMock()))
        observation.watch_socket(socket)
        await socket.send_json({'t': 'printer', 'action': 'status'})
        await socket.send_json({'t': 'printer_image', 'action': 'start'})
        await socket.send_bytes(b'\x06abc')
        self.assertEqual((observation.starts, observation.bands), (1, 1))
        with self.assertRaises(AssertionError):
            await socket.send_json({'t': 'printer_image', 'action': 'start'})
        with self.assertRaises(AssertionError):
            await socket.send_json({'t': 'printer', 'action': 'print'})
        self.assertEqual(socket.send_json.__closure__ is None, False)
        observation.restore()
        self.assertEqual(socket.send_json.await_count, 2)


class LiveImageFixtureSafety(unittest.IsolatedAsyncioTestCase):
    def live_observations(self):
        state = {'heap_failures': 0, 'heap_internal': 12000, 'heap_dma': 8000,
                 'printer_busy': True, 'printer_preview_active': True, 'printer_preview_presented': True,
                 'live_active': True, 'mic_open': True, 'mic_rx_enabled': True, 'online': True}
        return SimpleNamespace(errors=[], start_at=1.0, end_at=3.0,
            states=[{'at': 1.5, **state}, {'at': 2.5, **state}],
            connection=SimpleNamespace(mic_times=[.9, 2.0, 3.1]))

    def test_requires_mic_during_presented_preview_and_unchanged_heap(self):
        from device_image import verify_live_image
        observed = self.live_observations()
        self.assertEqual(verify_live_image(observed, {'heap_failures': 0})['mic_frames_during_preview'], 1)
        observed.connection.mic_times = [1.4, 2.6]
        with self.assertRaisesRegex(AssertionError, 'No Live microphone frame'):
            verify_live_image(observed, {'heap_failures': 0})
        observed.connection.mic_times = [2.0]
        observed.states[0]['heap_failures'] = 1
        with self.assertRaisesRegex(AssertionError, 'Heap allocation'):
            verify_live_image(observed, {'heap_failures': 0})

    def test_missing_preview_or_mic_state_never_confirms_overlap(self):
        from device_image import verify_live_image
        observed = self.live_observations()
        observed.states[0]['printer_preview_presented'] = False
        with self.assertRaisesRegex(AssertionError, 'sustained'):
            verify_live_image(observed, {'heap_failures': 0})
        observed = self.live_observations()
        observed.states[0]['mic_rx_enabled'] = False
        with self.assertRaisesRegex(AssertionError, 'microphone'):
            verify_live_image(observed, {'heap_failures': 0})

    async def test_reconnect_keeps_single_print_guard(self):
        from device_image import LiveObservations
        socket = SimpleNamespace(send_json=AsyncMock(), send_bytes=AsyncMock())
        peer = SimpleNamespace()
        adapter = SimpleNamespace(command=AsyncMock(), authenticate=AsyncMock(return_value=peer))
        observed = LiveObservations(adapter)
        observed.install()
        await adapter.authenticate(socket)
        await socket.send_json({'t': 'printer_image', 'action': 'start', 'rid': 'a' * 32})
        second = SimpleNamespace(send_json=AsyncMock(), send_bytes=AsyncMock())
        await adapter.authenticate(second)
        with self.assertRaisesRegex(AssertionError, 'second physical image'):
            await second.send_json({'t': 'printer_image', 'action': 'start', 'rid': 'b' * 32})
        observed.restore()

    def test_mode_is_explicit_and_mutually_exclusive(self):
        from device import acceptance, arguments, printer_mode
        from unittest.mock import patch
        argv = ['device.py', '--source', '/tmp/hermes', '--tls-identity', '/tmp/tls.json',
                '--control', 'http://127.0.0.1:18792', '--evidence', '/tmp/new-evidence']
        with patch('sys.argv', argv + ['--printer-live-image-check']):
            options = arguments()
        self.assertTrue(printer_mode(options))
        self.assertEqual(acceptance(options)[0].__name__, 'LiveObservations')
        with patch('sys.argv', argv + ['--printer-live-image-check', '--printer-image-check']):
            with self.assertRaises(SystemExit):
                arguments()

class VerifiedImageFixtureSafety(unittest.IsolatedAsyncioTestCase):
    def test_verified_uses_profile_dimensions_and_synthetic_remains_bounded(self):
        from device_image import print_arguments
        ref = 'a' * 32
        self.assertEqual(print_arguments(ref, False, True), {'action': 'print', 'format': 'image', 'image_ref': ref})
        self.assertEqual(print_arguments(ref, False, False)['width_mm'], 20)
        self.assertEqual(print_arguments(ref, False, False)['height_mm'], 20)
        self.assertEqual(print_arguments(ref, True, False)['format'], 'document')

    def test_verified_source_requires_matching_owner_digest_and_regular_file(self):
        import hashlib, tempfile
        from pathlib import Path
        from PIL import Image
        from device_image import prepare_fixture_image
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, target = root / 'source.png', root / 'target.png'
            Image.new('RGB', (12, 18), 'red').save(source)
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
            peer = SimpleNamespace(identity='kubik:owner')
            control = SimpleNamespace(printer_image_source=(source, digest, peer.identity))
            result = prepare_fixture_image(target, peer, control, True)
            self.assertEqual(result['source_sha256'], digest)
            self.assertEqual(target.read_bytes(), source.read_bytes())
            for bad in ((source, digest, 'kubik:other'), (source, '0' * 64, peer.identity)):
                control.printer_image_source = bad
                with self.assertRaises(ValueError):
                    prepare_fixture_image(target, peer, control, True)
            alias = root / 'link.png'
            alias.symlink_to(source)
            control.printer_image_source = (alias, digest, peer.identity)
            with self.assertRaises(OSError):
                prepare_fixture_image(target, peer, control, True)

    async def test_reconnect_does_not_reset_one_print_budget(self):
        from device_image import VerifiedObservations
        peer = SimpleNamespace()
        adapter = SimpleNamespace(command=AsyncMock(), authenticate=AsyncMock(return_value=peer))
        observation = VerifiedObservations(adapter)
        observation.install()
        first = SimpleNamespace(send_json=AsyncMock(), send_bytes=AsyncMock())
        await adapter.authenticate(first)
        await first.send_json({'t': 'printer_image', 'action': 'start'})
        second = SimpleNamespace(send_json=AsyncMock(), send_bytes=AsyncMock())
        await adapter.authenticate(second)
        with self.assertRaisesRegex(AssertionError, 'second physical image'):
            await second.send_json({'t': 'printer_image', 'action': 'start'})
        observation.restore()

    def test_verified_mode_requires_all_provenance_and_is_exclusive(self):
        from device import acceptance, arguments, printer_mode
        from unittest.mock import patch
        base = ['device.py', '--source', '/tmp/hermes', '--tls-identity', '/tmp/tls.json',
                '--control', 'http://127.0.0.1:18792', '--evidence', '/tmp/new-evidence']
        flags = ['--printer-verified-image-check', '--printer-image-source', '/tmp/user.png',
                 '--printer-image-source-sha256', 'a' * 64, '--printer-image-source-owner', 'kubik:owner']
        with patch('sys.argv', base + flags):
            options = arguments()
        self.assertTrue(printer_mode(options))
        self.assertEqual(acceptance(options)[0].__name__, 'VerifiedObservations')
        for partial in (flags[:1], flags[:-1], flags + ['--printer-image-check']):
            with patch('sys.argv', base + partial):
                with self.assertRaises(SystemExit):
                    arguments()
