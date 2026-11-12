"""Image origin, streaming order, cancellation and connection-bound print tests."""
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace

from hermes_test_support import DeviceSocket, KubikAdapter, Peer, ProtocolFixture, atomic_json
from hermes_printer_support import PRINTER
from hermes_kubik.printer_image_origin import ImageOrigin
from hermes_kubik.printer_image_raster import Raster
from hermes_kubik.printer_jobs import PrinterJobs
from hermes_kubik.native_jobs import NativeJob, classic_deadline


class ImageOriginTest(ProtocolFixture):
    async def test_only_generated_local_png_is_snapshotted_and_connection_scoped(self):
        directory = Path(self.temp.name) / 'cache/generated/images'
        directory.mkdir(parents=True)
        path = directory / 'ok.png'; path.write_bytes(b'original')
        origin = ImageOrigin()
        value = {'success': True, 'provider': 'openai', 'image': str(path)}
        ref = origin.capture(self.temp.name, 'call', json.dumps(value))
        self.assertEqual(origin.get(ref), b'original')
        self.assertEqual(origin.capture(self.temp.name, 'call', value), ref)
        path.write_bytes(b'changed')
        self.assertIsNone(origin.capture(self.temp.name, 'call', value))
        link = directory / 'link.png'; link.symlink_to(path)
        for image in (str(link), '/etc/passwd', 'https://example.com/image.png'):
            self.assertIsNone(origin.capture(self.temp.name, 'other', {**value, 'image': image}))
        self.assertIsNone(origin.capture(self.temp.name, 'other', {**value, 'provider': 'other'}))
        origin.close()
        self.assertIsNone(origin.get(ref))
        self.assertIsNone(origin.capture(self.temp.name, 'new', value))

    async def test_bounded_retention_expiry_and_oldest_eviction(self):
        directory = Path(self.temp.name) / 'cache/generated/images'
        directory.mkdir(parents=True)
        path = directory / 'ok.png'; path.write_bytes(b'png')
        value = {'success': True, 'provider': 'openai', 'image': str(path)}
        clock = [0]
        origin = ImageOrigin(now=lambda: clock[0])
        refs = [origin.capture(self.temp.name, str(i), value) for i in range(5)]
        self.assertIsNone(origin.get(refs[0]))
        self.assertEqual(origin.get(refs[1]), b'png')
        clock[0] = 600
        self.assertTrue(all(origin.get(ref) is None for ref in refs))
        self.assertEqual(len(origin.calls), 0)

    async def test_expired_image_is_cleared_without_further_access(self):
        directory = Path(self.temp.name) / 'cache/generated/images'
        directory.mkdir(parents=True)
        path = directory / 'ok.png'; path.write_bytes(b'png')
        origin = ImageOrigin(loop=asyncio.get_running_loop(), ttl=.03)
        ref = origin.capture(self.temp.name, 'call', {'success': True, 'provider': 'openai', 'image': str(path)})
        origin.mark_uncertain(ref)
        self.assertTrue(origin.is_uncertain(ref))
        await asyncio.sleep(.08)
        self.assertEqual(origin.images, {})
        self.assertEqual(origin.calls, {})
        self.assertFalse(origin.is_uncertain(ref))


class ImageJobsTest(ProtocolFixture):
    def setUp(self):
        super().setUp()
        self.adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
        self.adapter.security = self.store
        identity, _ = self.store.identity(self.hello)
        atomic_json(self.store.allow_path, [identity])
        self.peer = Peer(DeviceSocket(), identity, {**self.hello, 'printer': 2, 'printer_image': 1})
        self.peer.ready = True
        self.adapter.peers[identity] = self.peer
        self.jobs = self.adapter.printer_jobs = PrinterJobs(self.adapter, print_timeout=.1, hard_guard=.2)
        self.raster = Raster(bytes(range(72)) * 20, 576, 100, 20)
        self.args = {'action': 'print', 'format': 'image', 'image_ref': 'a' * 32}
        self.blocked = []
        self.peer.printer_images = SimpleNamespace(mark_uncertain=self.blocked.append,
                                                   is_uncertain=lambda ref: ref in self.blocked, get=lambda ref: b'png')

    async def asyncTearDown(self):
        await self.jobs.shutdown()
        await super().asyncTearDown()

    async def wait_for(self, predicate):
        async with asyncio.timeout(1):
            while not predicate():
                await asyncio.sleep(.001)

    async def test_native_deadline_extends_only_with_a_bounded_ceiling(self):
        loop = asyncio.get_running_loop()
        now = loop.time()
        async with asyncio.timeout(240) as deadline:
            job = SimpleNamespace(deadline=deadline, started=now)
            self.assertTrue(NativeJob.extend_for_document(job, 420))
            self.assertGreaterEqual(deadline.when(), now + 419)
            previous = deadline.when()
            self.assertFalse(NativeJob.extend_for_document(job, 1000))
            self.assertEqual(deadline.when(), previous)

    async def test_classic_voice_deadline_extends_for_the_same_document_turn(self):
        self.peer.message_id = 'classic-event'
        async with classic_deadline(self.peer, 'classic-event'):
            deadline, started = self.peer.classic_deadlines['classic-event']
            original = deadline.when()
            self.jobs.extend_native_document(self.peer, 'other-event', {'format': 'document'}, 410)
            self.assertEqual(deadline.when(), original)
            self.jobs.extend_native_document(self.peer, 'classic-event', {'format': 'document'}, 410)
            self.assertGreaterEqual(deadline.when(), started + 419)
        self.assertEqual(self.peer.classic_deadlines, {})

    async def test_overlapping_classic_turns_keep_separate_deadlines(self):
        async with classic_deadline(self.peer, 'first'):
            first, started = self.peer.classic_deadlines['first']
            async with classic_deadline(self.peer, 'second'):
                second, _ = self.peer.classic_deadlines['second']
                previous = second.when()
                self.jobs.extend_native_document(self.peer, 'first', {'format': 'document'}, 410)
                self.assertGreaterEqual(first.when(), started + 419)
                self.assertEqual(second.when(), previous)
            self.assertIn('first', self.peer.classic_deadlines)
            self.assertNotIn('second', self.peer.classic_deadlines)
        self.assertEqual(self.peer.classic_deadlines, {})

    async def test_only_document_extends_corresponding_native_job(self):
        calls = []
        job = SimpleNamespace(peer=self.peer, extend_for_document=lambda value: calls.append(value) or True)
        self.adapter.native_jobs.jobs['event'] = job
        self.jobs.extend_native_document(self.peer, 'event', {'format': 'image'}, 40)
        self.assertEqual(calls, [])
        self.jobs.extend_native_document(self.peer, 'event', {'format': 'document'}, 410)
        self.assertEqual(calls, [420])
        self.adapter.native_jobs.jobs.clear()

    async def test_credit_stream_and_same_call_do_not_repeat_paper(self):
        task = asyncio.create_task(self.jobs.request(self.peer, 'call', self.args, 'event', self.raster))
        await self.wait_for(lambda: bool(self.peer.socket.messages))
        start = self.peer.socket.messages[0]
        self.assertEqual((start['t'], start['height']), ('printer_image', 20))
        for offset, size in ((0, 16), (16, 4)):
            await self.adapter.command(self.peer, {'t': 'printer_image_ready', 'rid': start['rid'], 'offset': offset})
            await self.wait_for(lambda: len(self.peer.socket.messages) > 1 + (offset > 0))
            frame = self.peer.socket.messages[-1]
            self.assertEqual(frame[:2], b'\x06\x00')
            self.assertEqual(int.from_bytes(frame[34:36], 'big'), offset)
            self.assertEqual(frame[36], size)
            self.assertEqual(frame[37:], self.raster.block(offset, size))
        layout = {'format': 'image', 'width_dots': 100, 'height_dots': 20,
                  'width_mm': 100 * 25.4 / 304, 'height_mm': 20 * 25.4 / 225}
        await self.adapter.command(self.peer, {'t': 'printer_result', 'rid': start['rid'], 'status': 'sent',
                                               'outcome': 'submitted', 'printer': PRINTER, 'layout': layout})
        value = await task
        self.assertEqual(value['outcome'], 'submitted')
        self.assertFalse(value['paperConfirmed'])
        self.assertEqual(await self.jobs.request(self.peer, 'call', self.args, 'event', self.raster), value)
        self.assertEqual(len(self.peer.socket.messages), 3)

    async def test_mixed_document_is_one_stream_and_returns_document_layout(self):
        ref = self.args['image_ref']
        args = {'action': 'print', 'format': 'document', 'blocks': [
            {'type': 'text', 'text': 'Вступление'}, {'type': 'image', 'image_ref': ref},
            {'type': 'text', 'text': 'Заключение'}]}
        task = asyncio.create_task(self.jobs.request(self.peer, 'document', args, 'event', self.raster))
        await self.wait_for(lambda: bool(self.peer.socket.messages))
        start = self.peer.socket.messages[0]
        for offset in (0, 16):
            await self.adapter.command(self.peer, {'t': 'printer_image_ready', 'rid': start['rid'], 'offset': offset})
            await self.wait_for(lambda: len(self.peer.socket.messages) > 1 + (offset > 0))
        layout = {'format': 'image', 'width_dots': 100, 'height_dots': 20,
                  'width_mm': 100 * 25.4 / 304, 'height_mm': 20 * 25.4 / 225}
        await self.adapter.command(self.peer, {'t': 'printer_result', 'rid': start['rid'], 'status': 'sent',
                                               'outcome': 'submitted', 'printer': PRINTER, 'layout': layout})
        value = await task
        self.assertEqual(value['layout']['format'], 'document')
        self.assertEqual((value['pagesSubmitted'], value['pagesTotal']), (1, 1))
        self.assertEqual(len(self.peer.socket.messages), 3)
        self.assertEqual(await self.jobs.request(self.peer, 'document', args, 'event', self.raster), value)
        self.assertEqual(len(self.peer.socket.messages), 3)

    async def test_unknown_text_only_document_is_not_repeated_with_new_call_id(self):
        args = {'action': 'print', 'format': 'document', 'blocks': [{'type': 'text', 'text': 'Статья'}]}
        value = await self.jobs.request(self.peer, 'doc-1', args, 'event', self.raster)
        self.assertEqual(value['outcome'], 'unknown')
        await self.jobs.break_connection(self.peer)
        await self.wait_for(lambda: not self.jobs.pending)
        replacement = Peer(DeviceSocket(), self.peer.identity, {**self.hello, 'printer': 2, 'printer_image': 1})
        replacement.ready = True
        self.adapter.peers[self.peer.identity] = replacement
        value = await self.jobs.request(replacement, 'doc-2', args, 'event', self.raster)
        self.assertFalse(replacement.socket.messages)
        self.assertEqual(value['code'], 'outcome_unknown')
        self.assertEqual(len([m for m in self.peer.socket.messages if isinstance(m, dict) and m.get('action') == 'start']), 1)

    async def answer_page(self, start, *, fail=False):
        if fail:
            await self.adapter.command(self.peer, {'t': 'printer_result', 'rid': start['rid'], 'status': 'error',
                                                   'code': 'printer_not_found', 'outcome': 'not_sent'})
            return
        for offset in (0, 16):
            await self.adapter.command(self.peer, {'t': 'printer_image_ready', 'rid': start['rid'], 'offset': offset})
            await self.wait_for(lambda: len([m for m in self.peer.socket.messages if isinstance(m, bytes)
                                            and m[2:34].decode() == start['rid']]) > (offset > 0))
        layout = {'format': 'image', 'width_dots': 100, 'height_dots': 20,
                  'width_mm': 100 * 25.4 / 304, 'height_mm': 20 * 25.4 / 225}
        await self.adapter.command(self.peer, {'t': 'printer_result', 'rid': start['rid'], 'status': 'sent',
                                               'outcome': 'submitted', 'printer': PRINTER, 'layout': layout})

    async def test_document_pages_are_sequential_under_one_lease(self):
        args = {'action': 'print', 'format': 'document', 'blocks': [{'type': 'text', 'text': 'Длинная статья'}]}
        task = asyncio.create_task(self.jobs.request(self.peer, 'article', args, 'event', [self.raster, self.raster]))
        await self.wait_for(lambda: bool(self.peer.socket.messages))
        first = self.peer.socket.messages[0]
        await self.answer_page(first)
        await self.wait_for(lambda: len([m for m in self.peer.socket.messages if isinstance(m, dict)]) == 2)
        second = [m for m in self.peer.socket.messages if isinstance(m, dict)][-1]
        self.assertNotEqual(first['rid'], second['rid'])
        self.assertTrue(self.peer.output_lock.locked())
        await self.answer_page(second)
        result = await task
        self.assertEqual((result['status'], result['pagesSubmitted'], result['pagesTotal']), ('sent', 2, 2))
        self.assertEqual(result['layout']['format'], 'document')
        self.assertEqual(await self.jobs.request(self.peer, 'article', args, 'event', [self.raster, self.raster]), result)
        self.assertEqual(len([m for m in self.peer.socket.messages if isinstance(m, dict)]), 2)

    async def test_partial_document_blocks_retry_without_reprinting_first_page(self):
        args = {'action': 'print', 'format': 'document', 'blocks': [{'type': 'text', 'text': 'Статья'}]}
        task = asyncio.create_task(self.jobs.request(self.peer, 'article', args, 'event', [self.raster, self.raster]))
        await self.wait_for(lambda: bool(self.peer.socket.messages))
        await self.answer_page(self.peer.socket.messages[0])
        await self.wait_for(lambda: len([m for m in self.peer.socket.messages if isinstance(m, dict)]) == 2)
        await self.answer_page([m for m in self.peer.socket.messages if isinstance(m, dict)][-1], fail=True)
        result = await task
        self.assertEqual((result['outcome'], result['pagesSubmitted']), ('partial', 1))
        retry = await self.jobs.request(self.peer, 'new-call', args, 'event', [self.raster, self.raster])
        self.assertEqual((retry['code'], retry['outcome']), ('outcome_unknown', 'unknown'))
        self.assertEqual(len([m for m in self.peer.socket.messages if isinstance(m, dict)]), 2)
        replacement = Peer(DeviceSocket(), self.peer.identity, {**self.hello, 'printer': 2, 'printer_image': 1})
        replacement.ready = True; self.adapter.peers[self.peer.identity] = replacement
        retry = await self.jobs.request(replacement, 'after-reconnect', args, 'event', [self.raster, self.raster])
        self.assertEqual(retry['code'], 'outcome_unknown')
        self.assertFalse(replacement.socket.messages)

    async def test_second_page_timeout_reports_prior_submissions_without_replay(self):
        self.jobs.hard_guard = .08
        args = {'action': 'print', 'format': 'document', 'blocks': [{'type': 'text', 'text': 'Статья'}]}
        task = asyncio.create_task(self.jobs.request(self.peer, 'article-timeout', args, 'event', [self.raster, self.raster]))
        await self.wait_for(lambda: bool(self.peer.socket.messages))
        await self.answer_page(self.peer.socket.messages[0])
        await self.wait_for(lambda: len([m for m in self.peer.socket.messages if isinstance(m, dict)]) == 2)
        result = await task
        self.assertEqual((result['outcome'], result['pagesSubmitted'], result['pagesTotal']), ('unknown', 1, 2))
        await self.wait_for(lambda: not self.jobs.pending)
        self.peer.socket.closed = False; self.peer.ready = True
        retry = await self.jobs.request(self.peer, 'other-call', args, 'event', [self.raster, self.raster])
        self.assertEqual(retry['code'], 'outcome_unknown')
        self.assertEqual(len([m for m in self.peer.socket.messages if isinstance(m, dict)]), 2)

    async def test_prepared_image_cannot_dispatch_after_other_call_becomes_unknown(self):
        # Simulates a concurrent caller that already passed status and prepared raster.
        self.blocked.append(self.args['image_ref'])
        value = await self.jobs.request(self.peer, 'new-call', self.args, 'event', self.raster)
        self.assertEqual((value['code'], value['outcome']), ('outcome_unknown', 'unknown'))
        self.assertFalse(self.peer.socket.messages)

    async def test_cancel_before_dispatch_does_not_block_image(self):
        await self.peer.output_lock.acquire()
        task = asyncio.create_task(self.jobs.request(self.peer, 'cancel-early', self.args, 'event', self.raster))
        await self.wait_for(lambda: self.peer.identity in self.jobs.pending)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertEqual(self.blocked, [])
        self.assertFalse(self.peer.socket.messages)
        self.peer.output_lock.release()

    async def test_timeout_aborts_without_replay_and_keeps_output_lease(self):
        task = asyncio.create_task(self.jobs.request(self.peer, 'call', self.args, 'event', self.raster))
        value = await task
        self.assertEqual((value['code'], value['outcome']), ('timeout', 'unknown'))
        self.assertEqual(self.peer.socket.messages[-1]['action'], 'abort')
        self.assertTrue(self.peer.output_lock.locked())
        self.assertEqual(self.blocked, [self.args['image_ref']])
        self.assertEqual(await self.jobs.request(self.peer, 'call', self.args, 'event', self.raster), value)
        self.assertEqual(len(self.peer.socket.messages), 2)
