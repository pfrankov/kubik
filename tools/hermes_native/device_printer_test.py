"""Physical printer harness guards; no USB, BLE, paper or fabricated printer replies."""
import json
import asyncio
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, patch

from device_printer import (LiveObservations, MicConnection, Observations, PLAN, STREAM_PLAN, StreamObservations, cached_call,
                            monitor_live, verify_live_window, wait_discovery)
from printer_contract import printer_answer, printer_arguments


class PrinterHarnessTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.evidence = Path(self.directory.name)
        self.socket = SimpleNamespace(send_json=AsyncMock())
        self.adapter = SimpleNamespace(command=AsyncMock(), authenticate=AsyncMock(),
                                       printer_jobs=SimpleNamespace(request=AsyncMock()))
        self.observations = Observations(self.adapter)
        self.observations.evidence = self.evidence
        self.observations.install()
        self.observations.watch_socket(self.socket)

    async def asyncTearDown(self):
        self.observations.restore()
        self.directory.cleanup()

    async def send_plan(self, index):
        value = {'t': 'printer', 'rid': str(index) * 32, **PLAN[index]}
        if index:
            value.setdefault('format', 'text')
        await self.socket.send_json(value)

    async def test_expected_budget_and_repeat_guard_are_persisted_before_dispatch(self):
        await self.send_plan(0)
        await self.send_plan(1)
        with self.assertRaisesRegex(AssertionError, 'repeated'):
            await self.send_plan(1)
        await self.send_plan(2)
        with self.assertRaisesRegex(AssertionError, 'repeated'):
            await self.send_plan(2)
        self.assertEqual(len(self.observations.requests()), 3)
        path = self.evidence / 'printer-wire.json'
        self.assertEqual(len(json.loads(path.read_text())), 3)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        original = self.observations.senders[0][1]
        self.assertEqual(original.await_count, 3)

    async def test_cache_proof_uses_exact_original_call_and_checks_no_new_wire(self):
        row = {'call_id': 'actual-id', 'event_id': 'actual-event', 'arguments': PLAN[1],
               'result': {'rid': 'a' * 32, 'status': 'sent', 'outcome': 'submitted'}}
        self.observations.request.return_value = row['result']
        peer = object()
        await cached_call(peer, self.observations, row)
        self.observations.request.assert_awaited_once_with(peer, 'actual-id', PLAN[1], 'actual-event')
        self.assertEqual(self.observations.cache[0]['extra_device_requests'], 0)

    async def test_auto_discovery_must_be_true_and_quiescent_for_continuous_half_second(self):
        states = [{'printer_busy': busy, 'printer_discovered': found} for busy, found in
                  ((False, False), (False, True), (True, True), (False, True), (False, True), (False, True))]
        async def until(predicate, label, timeout):
            self.assertEqual(timeout, 40)
            results = [predicate(state) for state in states]
            self.assertEqual(results, [False, False, False, False, False, True])
        control = SimpleNamespace(evidence=self.evidence, until=until)
        with patch('device_printer.time.monotonic', side_effect=(0, 1, 1.2, 2, 2.4, 2.6)):
            await wait_discovery(control)
        self.assertEqual(len(json.loads((self.evidence / 'printer-discovery.json').read_text())), 6)

    async def test_missing_discovery_diagnostic_fails_instead_of_using_manual_status(self):
        async def until(predicate, *_args, **_kwargs):
            predicate({'printer_busy': False})
        with self.assertRaisesRegex(AssertionError, 'diagnostics missing'):
            await wait_discovery(SimpleNamespace(evidence=self.evidence, until=until))
        self.assertTrue((self.evidence / 'printer-discovery.json').is_file())

    def test_printer_option_is_explicit(self):
        from device import arguments
        argv = ['device.py', '--source', '/tmp/hermes', '--tls-identity', '/tmp/tls.json',
                '--control', 'http://127.0.0.1:18792', '--evidence', '/tmp/new-evidence', '--printer']
        with patch('sys.argv', argv):
            self.assertTrue(arguments().printer)

    async def test_live_plan_rejects_every_print_and_second_status_before_forwarding(self):
        self.observations.restore()
        self.observations = LiveObservations(self.adapter)
        self.observations.evidence = self.evidence
        self.observations.install()
        self.observations.watch_socket(self.socket)
        with self.assertRaisesRegex(AssertionError, 'repeated'):
            await self.send_plan(1)
        await self.send_plan(0)
        with self.assertRaisesRegex(AssertionError, 'repeated'):
            await self.send_plan(1)
        with self.assertRaisesRegex(AssertionError, 'repeated'):
            await self.send_plan(0)
        self.assertEqual([row['action'] for row in self.observations.requests()], ['status'])
        self.assertEqual(self.observations.senders[0][1].await_count, 1)

    def test_live_status_marker_yields_unique_genuine_tool_call_id(self):
        first, second = '0123456789abcdef', 'fedcba9876543210'
        for token in (first, second):
            marker = 'kubik-native-printer-live-status-' + token
            self.assertEqual(printer_arguments(marker), ('live-status-' + token, {'action': 'status'}))
            answer, finish = printer_answer([{'role': 'user', 'content': marker}])
            call = answer['tool_calls'][0]
            self.assertEqual(finish, 'tool_calls')
            self.assertEqual(call['id'], 'call-printer-live-status-' + token)
            self.assertEqual(json.loads(call['function']['arguments']), {'action': 'status'})

    def live_window(self):
        state = {'heap_failures': 0, 'heap_internal': 12000, 'heap_dma': 10000,
                 'live_active': True, 'mic_open': True, 'mic_rx_enabled': True, 'online': True,
                 'phase': 'live'}
        return SimpleNamespace(requests=lambda: [{'rid': 'abc', 'action': 'status'}],
            wire=[{'direction': 'host', 'at': 1, 'value': {'rid': 'abc'}},
                  {'direction': 'device', 'at': 2, 'value': {'rid': 'abc'}}],
            connection=SimpleNamespace(mic_times=[.9, 1.5, 2.1]), states=[state], errors=[], voice_wire=[])

    def test_live_window_requires_actual_in_window_input_and_unchanged_heap_failures(self):
        observed = self.live_window()
        result = verify_live_window(observed, {'heap_failures': 0})
        self.assertEqual(result['mic_frames_during_rpc'], 1)
        self.assertEqual(result['heap_internal_min'], 12000)
        observed.connection.mic_times = [.9, 2.1]
        with self.assertRaisesRegex(AssertionError, 'during'):
            verify_live_window(observed, {'heap_failures': 0})
        observed.connection.mic_times = [1.5]
        observed.states[0]['heap_failures'] = 1
        with self.assertRaises(AssertionError):
            verify_live_window(observed, {'heap_failures': 0})

    async def test_live_monitor_persists_usb_failure_and_mic_arrival_times(self):
        observed = LiveObservations(self.adapter)
        observed.evidence = self.evidence
        observed.connection = MicConnection('live', AsyncMock())
        await observed.connection.respond({'type': 'session.input_audio.append'})
        control = SimpleNamespace(info=AsyncMock(side_effect=RuntimeError('USB unavailable')))
        await monitor_live(control, observed, asyncio.Event())
        self.assertEqual(observed.errors[0]['type'], 'RuntimeError')
        self.assertEqual(len(json.loads((self.evidence / 'printer-live-input.json').read_text())), 1)
        self.assertTrue((self.evidence / 'printer-live-errors.json').is_file())

    def test_live_status_option_is_mutually_exclusive_with_printing(self):
        from device import arguments
        argv = ['device.py', '--source', '/tmp/hermes', '--tls-identity', '/tmp/tls.json',
                '--control', 'http://127.0.0.1:18792', '--evidence', '/tmp/new-evidence', '--printer-live-status']
        with patch('sys.argv', argv):
            args = arguments()
            self.assertTrue(args.printer_live_status)
            self.assertFalse(args.printer)
        with patch('sys.argv', argv + ['--printer']), patch('sys.stderr'):
            with self.assertRaises(SystemExit):
                arguments()

    async def test_stream_check_permits_only_the_new_single_job(self):
        self.observations.restore()
        self.observations = StreamObservations(self.adapter)
        self.observations.evidence = self.evidence
        self.observations.install()
        self.observations.watch_socket(self.socket)
        for index in range(3):
            with self.assertRaisesRegex(AssertionError, 'repeated'):
                await self.send_plan(index)
        value = {'t': 'printer', 'rid': 'a' * 32, **STREAM_PLAN, 'format': 'text'}
        await self.socket.send_json(value)
        with self.assertRaisesRegex(AssertionError, 'repeated'):
            await self.socket.send_json(value)
        self.assertEqual(self.observations.requests(), [value])
        self.assertEqual(self.observations.senders[0][1].await_count, 1)
        marker = 'kubik-printer-stream-check-0123456789abcdef'
        answer, _ = printer_answer([{'role': 'user', 'content': marker}])
        call = answer['tool_calls'][0]
        self.assertEqual(call['id'], 'call-printer-stream-check-0123456789abcdef')
        self.assertEqual(json.loads(call['function']['arguments']), STREAM_PLAN)

    def test_stream_check_cannot_be_combined_with_another_mode(self):
        from device import arguments
        argv = ['device.py', '--source', '/tmp/hermes', '--tls-identity', '/tmp/tls.json',
                '--control', 'http://127.0.0.1:18792', '--evidence', '/tmp/new-evidence', '--printer-stream-check']
        with patch('sys.argv', argv):
            self.assertTrue(arguments().printer_stream_check)
        for other in ('--printer', '--printer-live-status'):
            with patch('sys.argv', argv + [other]), patch('sys.stderr'):
                with self.assertRaises(SystemExit):
                    arguments()


if __name__ == '__main__':
    unittest.main()
