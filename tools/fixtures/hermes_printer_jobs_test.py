"""The real adapter routes print replies without replaying ambiguous side effects."""
import asyncio
from types import SimpleNamespace

from hermes_test_support import DeviceSocket, KubikAdapter, Peer, ProtocolFixture, atomic_json
from hermes_printer_support import reply
from hermes_emoji_support import atlas
from hermes_kubik.printer_jobs import PrinterJobs
from hermes_kubik.native_playback import NativePlayback


async def eventually(predicate):
    async with asyncio.timeout(1):
        while not predicate():
            await asyncio.sleep(.001)


class PrinterRequests(ProtocolFixture):
    def setUp(self):
        super().setUp()
        self.adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
        self.adapter.security = self.store
        self.identity, _ = self.store.identity(self.hello)
        atomic_json(self.store.allow_path, [self.identity])
        self.peer = Peer(DeviceSocket(), self.identity, {**self.hello, 'printer': 2})
        self.peer.ready = True
        self.adapter.peers[self.identity] = self.peer
        self.jobs = self.adapter.printer_jobs = PrinterJobs(self.adapter, status_timeout=.03, print_timeout=.03)

    async def asyncTearDown(self):
        await self.jobs.shutdown()
        await super().asyncTearDown()

    def start(self, call='call-1', params=None):
        return asyncio.create_task(self.jobs.request(self.peer, call, params or {'action': 'status'}, 'event-1'))

    async def answer(self, task):
        await eventually(lambda: self.identity in self.jobs.pending and bool(self.peer.socket.messages) and
                         self.peer.socket.messages[-1]['rid'] == self.jobs.pending[self.identity]['rid'])
        await self.adapter.command(self.peer, reply(self.peer.socket.messages[-1]))
        return await task

    async def test_status_and_print_metadata_take_the_authenticated_socket_path(self):
        status = await self.answer(self.start())
        self.assertEqual(status['status'], 'ready')
        self.assertEqual(status['printer']['width_dots'], 576)
        printed = await self.answer(self.start('call-2', {'action': 'print', 'text': 'Список', 'width_mm': 20}))
        self.assertEqual(printed['outcome'], 'submitted')
        self.assertAlmostEqual(printed['layout']['width_mm'], 20, delta=.084)
        self.assertFalse(printed['paperConfirmed'])
        self.assertEqual(self.peer.socket.messages[-1]['emoji'], [])
        self.assertEqual(self.jobs.pending, {})

    async def test_only_used_emoji_patterns_are_sent_on_the_authenticated_socket(self):
        self.jobs.atlas.path.parent.mkdir(parents=True, exist_ok=True)
        self.jobs.atlas.path.write_bytes(atlas())
        task = self.start(params={'action': 'print', 'text': 'Список 😀 ❤️ 😀'})
        result = await self.answer(task)
        self.assertEqual(result['outcome'], 'submitted')
        self.assertEqual([glyph['sequence'] for glyph in self.peer.socket.messages[-1]['emoji']], ['😀', '❤️'])

    async def test_missing_or_unsupported_composite_fails_not_sent_without_rpc(self):
        for index, text in enumerate(('😀', '👩‍💻')):
            result = await self.start(str(index), {'action': 'print', 'text': text})
            self.assertEqual((result['code'], result['outcome']), ('unsupported_character', 'not_sent'))
        self.assertFalse(self.peer.socket.messages)

    async def test_expired_clock_after_wire_preparation_cannot_dispatch(self):
        original = self.jobs.prepare_wire
        async def expired(row):
            await original(row)
            row['deadline'] = asyncio.get_running_loop().time() - 1
        self.jobs.prepare_wire = expired
        result = await self.start(params={'action': 'print', 'text': 'X'})
        self.assertEqual((result['code'], result['outcome']), ('timeout', 'not_sent'))
        self.assertFalse(self.peer.socket.messages)

    async def test_same_call_is_one_send_and_changed_geometry_is_a_conflict(self):
        args = {'action': 'print', 'text': 'X'}
        first = self.start(params=args)
        await eventually(lambda: bool(self.peer.socket.messages))
        duplicate = self.start(params={**args, 'format': 'text'})
        value = await self.answer(first)
        self.assertEqual(await duplicate, value)
        self.assertEqual(await self.start(params=args), value)
        changed = await self.start(params={**args, 'width_mm': 20})
        self.assertEqual(changed['code'], 'request_conflict')
        self.assertEqual(len(self.peer.socket.messages), 1)

    async def test_unknown_timeout_survives_cache_expiry_without_reprinting(self):
        args = {'action': 'print', 'text': 'X'}
        value = await self.start(params=args)
        self.assertEqual((value['code'], value['outcome']), ('timeout', 'unknown'))
        self.jobs.now = lambda: 10 ** 12
        self.assertEqual(await self.start(params=args), value)
        self.assertEqual(len(self.peer.socket.messages), 1)

    async def test_timeout_bounds_a_stalled_socket_send_as_well_as_its_reply(self):
        entered = asyncio.Event()
        async def send(_, **_kwargs):
            entered.set()
            await asyncio.Event().wait()
        self.peer.socket.send_json = send
        value = await asyncio.wait_for(self.start(params={'action': 'print', 'text': 'X'}), .2)
        self.assertTrue(entered.is_set())
        self.assertEqual((value['code'], value['outcome']), ('timeout', 'unknown'))
        self.assertTrue(self.jobs.pending)
        self.assertTrue(self.peer.output_lock.locked())

    async def test_version1_and_other_pending_request_are_not_sent(self):
        self.peer.printer_version = 1
        self.assertEqual((await self.start())['code'], 'printer_unsupported')
        self.assertFalse(self.peer.socket.messages)
        self.peer.printer_version = 2
        first = self.start('one')
        await eventually(lambda: bool(self.peer.socket.messages))
        self.assertEqual((await self.start('two'))['code'], 'printer_busy')
        await self.answer(first)

    async def test_authorization_is_checked_again_at_the_send_boundary(self):
        checks = iter((True, False))
        self.jobs.current = lambda peer: next(checks)
        value = await self.start(params={'action': 'print', 'text': 'X'})
        self.assertEqual((value['code'], value['outcome']), ('unauthorized', 'not_sent'))
        self.assertFalse(self.peer.socket.messages)

    async def test_revocation_replacement_and_late_reply_cannot_redirect_work(self):
        first = self.start(params={'action': 'print', 'text': 'X'})
        await eventually(lambda: bool(self.peer.socket.messages))
        old_request = self.peer.socket.messages[-1]
        replacement = Peer(DeviceSocket(), self.identity, {**self.hello, 'printer': 2})
        replacement.ready = True
        self.adapter.peers[self.identity] = replacement
        self.jobs.close(self.peer)
        self.assertEqual((await first)['outcome'], 'unknown')
        self.assertFalse(self.jobs.accept(replacement, reply(old_request)))
        self.assertEqual((await self.start('new'))['code'], 'device_replaced')
        self.peer = replacement
        atomic_json(self.store.allow_path, [])
        self.assertEqual((await self.start('revoked'))['code'], 'device_replaced')
        self.assertFalse(replacement.socket.messages)

    async def test_invalid_dimensions_do_not_become_a_successful_submission(self):
        task = self.start(params={'action': 'print', 'text': 'X', 'width_mm': 20})
        await eventually(lambda: bool(self.peer.socket.messages))
        request = self.peer.socket.messages[-1]
        await self.adapter.command(self.peer, reply({**request, 'width_mm': 40}))
        self.assertEqual((await task)['outcome'], 'unknown')
        self.assertEqual((await self.start(params={'action': 'print', 'text': 'X', 'width_mm': 20}))['code'], 'invalid_response')

    async def test_preflight_error_is_not_sent_and_cancellation_is_unknown(self):
        task = self.start(params={'action': 'print', 'text': 'X'})
        await eventually(lambda: bool(self.peer.socket.messages))
        request = self.peer.socket.messages[-1]
        await self.adapter.command(self.peer, {'t': 'printer_result', 'rid': request['rid'], 'status': 'error',
                                               'code': 'printer_not_found', 'outcome': 'not_sent'})
        self.assertEqual((await task)['outcome'], 'not_sent')
        task = self.start('cancel', {'action': 'print', 'text': 'Y'})
        await eventually(lambda: len(self.peer.socket.messages) == 2)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertEqual((await self.start('cancel', {'action': 'print', 'text': 'Y'}))['outcome'], 'unknown')

    async def test_timeout_waiting_for_audio_never_dispatches_later(self):
        await self.peer.output_lock.acquire()
        value = await self.start(params={'action': 'print', 'text': 'X'})
        self.assertEqual((value['code'], value['outcome']), ('timeout', 'not_sent'))
        self.peer.output_lock.release()
        await asyncio.sleep(.01)
        self.assertFalse(self.peer.socket.messages)
        self.assertFalse(self.jobs.pending)

    async def test_cancel_and_revoke_while_waiting_for_audio_cannot_dispatch(self):
        self.jobs.timeouts = {'status': .5, 'print': .5}
        await self.peer.output_lock.acquire()
        task = self.start(params={'action': 'print', 'text': 'X'})
        await eventually(lambda: bool(self.jobs.pending))
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertFalse(self.jobs.pending)
        task = self.start('revoked', {'action': 'print', 'text': 'Y'})
        await eventually(lambda: bool(self.jobs.pending))
        atomic_json(self.store.allow_path, [])
        self.peer.output_lock.release()
        value = await task
        self.assertEqual((value['code'], value['outcome']), ('unauthorized', 'not_sent'))
        self.assertFalse(self.peer.socket.messages)

    async def test_socket_close_releases_pending_lease_without_retry(self):
        args = {'action': 'print', 'text': 'X'}
        task = self.start(params=args)
        await eventually(lambda: bool(self.peer.socket.messages))
        await self.peer.socket.close()
        self.jobs.close(self.peer)
        value = await task
        self.assertEqual((value['code'], value['outcome']), ('disconnected', 'unknown'))
        await eventually(lambda: not self.jobs.pending)
        self.assertFalse(self.peer.output_lock.locked())
        self.assertEqual(len(self.peer.socket.messages), 1)

    async def test_cancellation_after_wire_keeps_audio_lease_until_late_valid_ack(self):
        args = {'action': 'print', 'text': 'X'}
        task = self.start(params=args)
        await eventually(lambda: bool(self.peer.socket.messages))
        request = self.peer.socket.messages[-1]
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        cached = await self.start(params=args)
        self.assertEqual(cached['outcome'], 'unknown')
        playback = NativePlayback(self.peer, lambda: True)
        playback.feed(b'\0' * 4800)
        finished = asyncio.create_task(playback.finish())
        await asyncio.sleep(.01)
        self.assertTrue(self.peer.output_lock.locked())
        self.assertFalse(any(isinstance(row, dict) and row['t'] == 'speak' for row in self.peer.socket.messages))
        self.assertEqual((await self.start('other'))['code'], 'printer_busy')
        await self.adapter.command(self.peer, reply(request))
        await eventually(lambda: self.peer.gen == 1 and self.peer.sent_ms == 100)
        await self.adapter.command(self.peer, {'t': 'played', 'gen': 1, 'ms': 100})
        await finished
        self.assertEqual(await self.start(params=args), cached)

    async def test_bad_ack_keeps_lease_and_good_late_ack_only_releases_it(self):
        args = {'action': 'print', 'text': 'X', 'width_mm': 20}
        task = self.start(params=args)
        await eventually(lambda: bool(self.peer.socket.messages))
        request = self.peer.socket.messages[-1]
        await self.adapter.command(self.peer, reply({**request, 'width_mm': 40}))
        cached = await task
        self.assertEqual(cached['code'], 'invalid_response')
        self.assertTrue(self.peer.output_lock.locked())
        await self.adapter.command(self.peer, reply(request))
        await eventually(lambda: not self.peer.output_lock.locked())
        self.assertEqual(await self.start(params=args), cached)

    async def test_disconnect_cleans_lease_and_hard_guard_closes_peer(self):
        self.jobs.hard_guard = .06
        value = await self.start(params={'action': 'print', 'text': 'X'})
        self.assertEqual(value['outcome'], 'unknown')
        self.assertTrue(self.peer.output_lock.locked())
        await eventually(lambda: self.peer.socket.closed)
        await eventually(lambda: not self.jobs.pending)
        self.assertFalse(self.peer.output_lock.locked())
        self.assertFalse(self.peer.ready)

    async def test_audio_drains_before_rpc_input_continues_and_queued_barge_does_not_deadlock(self):
        self.jobs.timeouts = {'status': .5, 'print': .5}
        original_send = self.peer.socket.send_json
        async def send(value, **kwargs):
            await original_send(value, **kwargs)
            if value['t'] == 'speak_cancel':
                await self.adapter.command(self.peer, {'t': 'cancelled', 'gen': value['gen']})
        self.peer.socket.send_json = send
        playback = NativePlayback(self.peer, lambda: True)
        playback.feed(b'\0' * 4800)
        drained = asyncio.create_task(playback.finish())
        await eventually(lambda: any(isinstance(row, dict) and row['t'] == 'speak_end' for row in self.peer.socket.messages))
        task = self.start()
        await asyncio.sleep(.01)
        self.assertFalse(any(isinstance(row, dict) and row['t'] == 'printer' for row in self.peer.socket.messages))
        await self.adapter.command(self.peer, {'t': 'played', 'gen': 1, 'ms': 100})
        await drained
        await eventually(lambda: isinstance(self.peer.socket.messages[-1], dict) and self.peer.socket.messages[-1]['t'] == 'printer')
        inputs = []
        self.peer.native = SimpleNamespace(microphone=inputs.append)
        self.adapter.microphone(self.peer, b'microphone-during-print')
        self.assertEqual(inputs, [b'microphone-during-print'])
        barged = NativePlayback(self.peer, lambda: True)
        barged.feed(b'\0' * 4800)
        await barged.cancel()
        after = NativePlayback(self.peer, lambda: True)
        after.feed(b'\0' * 4800)
        finished = asyncio.create_task(after.finish())
        await asyncio.sleep(.01)
        self.assertEqual(self.peer.gen, 1)
        await self.adapter.command(self.peer, reply(self.peer.socket.messages[-1]))
        self.assertEqual((await task)['status'], 'ready')
        await eventually(lambda: self.peer.gen == 2 and self.peer.sent_ms == 100)
        await self.adapter.command(self.peer, {'t': 'played', 'gen': 2, 'ms': 100})
        await finished
