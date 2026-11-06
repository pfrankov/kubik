"""Failure cleanup retains its transport until the baseline voice mode is observed."""
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import AsyncMock, patch

from device_restore import cleanup
from device_voice import Observations, timing_summary


class RestorationTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.events = []
        self.state = dict(voice_mode='live', mic_open=True, live_active=True,
                          screen_dark=False, menu=False)
        self.original = {'voice_mode': 'classic'}
        self.peer = SimpleNamespace(identity='approved', ready=True, native=None,
                                    socket=SimpleNamespace(closed=False, close_code=None))
        self.current = self.peer
        self.selected = None
        self.adapter = SimpleNamespace(_ready_peer=lambda identity: self.current)
        self.adapter.voice_controls = SimpleNamespace(
            store=SimpleNamespace(select=self.select), publish=self.publish)
        self.control = SimpleNamespace(evidence=Path(self.directory.name), sim=self.sim, info=self.info,
                                       until=self.until, restore=self.restore,
                                       http=SimpleNamespace(close=AsyncMock()))
        self.gateway = SimpleNamespace(stop=self.stop)

        async def close():
            self.assertFalse(self.state['mic_open'], 'Host closed before capture stopped')
            self.events.append('native_close')
            self.peer.native = None
        self.peer.native = SimpleNamespace(close=close)

    def select(self, identity, field, value):
        self.assertEqual((identity, field, value), ('approved', 'mode', 'classic'))
        self.events.append('select')
        self.selected = value

    async def publish(self, peer):
        self.assertIs(peer, self.current)
        self.assertFalse(peer.socket.closed)
        self.assertEqual(self.selected, 'classic')
        self.events.append('publish')
        self.state['voice_mode'] = self.selected

    async def sim(self, event, **fields):
        self.events.append(event)
        if event == 'menu':
            self.state.update(menu=True, mic_open=False, live_active=False)
        elif event == 'pwr':
            self.state['screen_dark'] = False
        else:
            self.assertEqual(event, 'ptt_up')

    async def info(self):
        return dict(self.state)

    async def until(self, predicate, label, timeout=30):
        self.assertTrue(predicate(self.state), label)
        return dict(self.state)

    async def stop(self):
        self.events.append('gateway_stop')

    async def restore(self, original, *, release_wifi_pause=True):
        self.events.append('route_restore')
        if self.state['voice_mode'] != original['voice_mode']:
            raise AssertionError('Original voice mode was not restored')

    async def run_cleanup(self, primary=None):
        await cleanup(self.gateway, self.adapter, self.peer, self.control, self.original,
                      None, None, None, primary)
        return json.loads((self.control.evidence / 'cleanup.json').read_text())

    async def test_capture_stops_and_mode_is_observed_before_transport_release(self):
        primary = AssertionError('Speaker gaps: 1')
        report = await self.run_cleanup(primary)
        self.assertTrue(report['ok'])
        self.assertTrue(report['mode_restored_before_release'])
        self.assertEqual(report['primary_error']['message'], 'Speaker gaps: 1')
        self.assertEqual(self.events, ['select', 'ptt_up', 'menu', 'native_close',
                                       'publish', 'gateway_stop', 'route_restore'])
        self.assertFalse(hasattr(primary, '__notes__'))

    async def test_closed_original_socket_restores_on_current_approved_connection(self):
        self.peer.socket.closed = True
        self.current = SimpleNamespace(identity='approved', ready=True, native=None,
                                       socket=SimpleNamespace(closed=False, close_code=None))
        report = await self.run_cleanup()
        self.assertTrue(report['ok'])
        self.assertTrue(report['initial_peer']['closed'])
        self.assertFalse(report['publish_attempts'][0]['closed'])
        self.assertLess(self.events.index('publish'), self.events.index('gateway_stop'))

    async def test_close_disconnect_is_recorded_and_replacement_still_restores(self):
        async def disconnect():
            self.peer.socket.closed = True
            self.current = SimpleNamespace(identity='approved', ready=True, native=None,
                                           socket=SimpleNamespace(closed=False, close_code=None))
            raise ConnectionError('Lost input shutdown transport')
        self.peer.native.close = disconnect
        report = await self.run_cleanup()
        self.assertTrue(report['mode_restored_before_release'])
        self.assertEqual(report['native_close_error']['type'], 'ConnectionError')

    async def test_disconnect_during_publish_gets_one_bounded_replacement_attempt(self):
        original_publish = self.publish
        attempts = 0
        async def publish(peer):
            nonlocal attempts
            attempts += 1
            if attempts == 1:
                peer.socket.closed = True
                self.current = SimpleNamespace(identity='approved', ready=True, native=None,
                                               socket=SimpleNamespace(closed=False, close_code=None))
                raise ConnectionError('Disconnected during capability publish')
            await original_publish(peer)
        self.adapter.voice_controls.publish = publish
        report = await self.run_cleanup()
        self.assertEqual(attempts, 2)
        self.assertEqual(len(report['publish_attempts']), 2)
        self.assertTrue(report['mode_restored_before_release'])

    async def test_failed_reconnect_keeps_original_failure_and_reports_cleanup_separately(self):
        primary = AssertionError('Speaker gaps: 1')
        with patch('device_restore.ready_peer', AsyncMock(side_effect=TimeoutError('No approved reconnect'))) as ready:
            report = await self.run_cleanup(primary)
        self.assertEqual(ready.await_count, 1)
        self.assertFalse(report['ok'])
        self.assertEqual(str(primary), 'Speaker gaps: 1')
        self.assertIn('No approved reconnect', primary.__notes__[0])
        self.assertIn('gateway_stop', self.events)
        self.assertIn('route_restore', self.events)


class SendTimingTests(unittest.TestCase):
    def test_summary_separates_generations_and_send_blocking(self):
        frames = [dict(gen=1, start=1, end=1.01, bytes=485),
                  dict(gen=1, start=1.6, end=1.62, bytes=485),
                  dict(gen=2, start=5, end=5.1, bytes=485)]
        result = timing_summary(frames)
        self.assertEqual(result['count'], 3)
        self.assertAlmostEqual(result['generations']['1']['longest_start_gap_ms'], 600)
        self.assertAlmostEqual(result['generations']['1']['longest_send_ms'], 20)
        self.assertEqual(result['generations']['2']['longest_start_gap_ms'], 0)
        self.assertEqual(result['frames'], frames)


class SendObservationTests(unittest.IsolatedAsyncioTestCase):
    async def test_observation_forwards_identical_payload_and_restores_socket(self):
        sender = AsyncMock(return_value='sent')
        socket = SimpleNamespace(send_bytes=sender)
        peer = SimpleNamespace(socket=socket)
        adapter = SimpleNamespace(authenticate=AsyncMock(return_value=peer), command=AsyncMock(), send=AsyncMock())
        echo = SimpleNamespace(process=lambda *_: b'')
        with patch('device_voice.importlib.import_module', return_value=SimpleNamespace(EchoCleaner=echo)):
            observation = Observations(adapter)
        observation.install()
        try:
            self.assertIs(await adapter.authenticate(socket), peer)
            with patch('device_voice.time.monotonic', side_effect=[1, 1.2]):
                self.assertEqual(await socket.send_bytes(b'\x03\x07abc'), 'sent')
            sender.assert_awaited_once_with(b'\x03\x07abc')
            self.assertEqual(observation.send_timing, [dict(start=1, end=1.2, bytes=5, gen=7)])
            observation.send_timing *= 4096
            with self.assertRaisesRegex(ValueError, 'timing observation limit'):
                await socket.send_bytes(b'\x03\x07abc')
            self.assertEqual(sender.await_count, 1)
        finally:
            observation.restore()
        self.assertIs(socket.send_bytes, sender)


if __name__ == '__main__':
    unittest.main()
