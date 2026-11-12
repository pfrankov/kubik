"""Attachments stay connection-bound and need a real device presentation receipt."""
import asyncio
from io import BytesIO
import os
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch

from PIL import Image
from hermes_test_support import DeviceSocket, KubikAdapter, Peer, ProtocolFixture, atomic_json
from hermes_kubik.printer_image_origin import ImageOrigin
from hermes_kubik import screen_image
from gateway.platforms.base import SendResult


class ScreenImageTest(ProtocolFixture):
    def setUp(self):
        super().setUp()
        self.adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
        self.adapter.security = self.store
        self.adapter.home = self.temp.name
        identity, _ = self.store.identity(self.hello)
        atomic_json(self.store.allow_path, [identity])
        self.peer = Peer(DeviceSocket(), identity, {**self.hello, 'screen_image': 1})
        self.peer.ready = True
        self.adapter.peers[identity] = self.peer
        self.peer.printer_images = ImageOrigin()
        directory = Path(self.temp.name) / 'cache/generated/images'
        directory.mkdir(parents=True)
        self.path = directory / 'fixture.png'
        Image.new('RGB', (40, 30), 'red').save(self.path)
        value = {'success': True, 'provider': 'openai', 'image': str(self.path)}
        self.ref = self.peer.printer_images.capture(self.temp.name, 'generation', value)
        self.assertIsNotNone(self.ref)

    def acknowledge(self, status='shown'):
        socket = self.peer.socket
        async def send(value, **_):
            socket.messages.append(value)
            if value.get('t') == 'screen_image':
                result = {'t': 'screen_image_result', 'rid': value['rid'], 'status': status}
                if status == 'error': result['code'] = 'busy'
                await self.adapter.command(self.peer, result)
        socket.send_json = send

    async def test_delivers_owner_image_with_confirmed_presentation_and_caption(self):
        self.acknowledge()
        self.adapter.send = AsyncMock(return_value=SendResult(success=True, message_id='caption'))
        result = await self.adapter.send_image_file(self.peer.identity, str(self.path), caption='A caption')
        self.assertTrue(result.success)
        self.adapter.send.assert_awaited_once_with(self.peer.identity, 'A caption', reply_to=None, metadata=None)
        messages = self.peer.socket.messages
        self.assertEqual(len(messages), 11)
        frames = messages[:-1]
        self.assertTrue(all(frame[:2] == b'\x07\x01' for frame in frames))
        rid = result.message_id
        self.assertTrue(all(frame[2:34] == rid.encode() for frame in frames))
        self.assertEqual([int.from_bytes(frame[34:36], 'big') for frame in frames], list(range(0, 8224, 896)))
        self.assertEqual(len(b''.join(frame[36:] for frame in frames)), 8224)
        self.assertEqual(messages[-1], {'t': 'screen_image', 'action': 'show', 'rid': rid})
        self.assertEqual(self.peer.image_receipts, {})

    async def test_foreign_expired_changed_and_symlink_paths_are_refused(self):
        self.acknowledge()
        foreign = self.path.parent / 'foreign.png'
        Image.new('RGB', (4, 4), 'blue').save(foreign)
        link = self.path.parent / 'link.png'; link.symlink_to(self.path)
        for path in (str(foreign), str(link), 'https://example.com/thing.png', '/etc/passwd'):
            self.assertFalse((await self.adapter.send_image_file(self.peer.identity, path)).success)
        Image.new('RGB', (40, 30), 'blue').save(self.path)
        self.assertFalse((await self.adapter.send_image_file(self.peer.identity, str(self.path))).success)
        self.assertFalse(self.peer.socket.messages)
        self.peer.printer_images.close()
        self.assertFalse((await self.adapter.send_image_file(self.peer.identity, str(self.path))).success)

    async def test_device_error_or_missing_ack_never_reports_success(self):
        self.acknowledge('error')
        result = await self.adapter.send_image_file(self.peer.identity, str(self.path))
        self.assertFalse(result.success)
        self.peer.socket.messages.clear()
        self.peer.socket.send_json = AsyncMock()
        with patch.object(screen_image, 'TIMEOUT', .02):
            result = await self.adapter.send_image_file(self.peer.identity, str(self.path))
        self.assertFalse(result.success)
        self.assertEqual(self.peer.image_receipts, {})

    async def test_replacement_during_lock_wait_never_receives_image(self):
        await self.peer.output_lock.acquire()
        task = asyncio.create_task(self.adapter.send_image_file(self.peer.identity, str(self.path)))
        await asyncio.sleep(.05)
        replacement = Peer(DeviceSocket(), self.peer.identity, {**self.hello, 'screen_image': 1})
        replacement.ready = True
        self.adapter.peers[self.peer.identity] = replacement
        self.peer.output_lock.release()
        result = await task
        self.assertFalse(result.success)
        self.assertFalse(self.peer.socket.messages)
        self.assertFalse(replacement.socket.messages)

    async def test_cancel_after_show_discards_pending_image_without_replay(self):
        sent = asyncio.Event()
        async def send(value, **_):
            self.peer.socket.messages.append(value)
            if value.get('action') == 'show':
                sent.set()
        self.peer.socket.send_json = send
        task = asyncio.create_task(self.adapter.send_image_file(self.peer.identity, str(self.path)))
        await asyncio.wait_for(sent.wait(), 1)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        messages = [value for value in self.peer.socket.messages if isinstance(value, dict)]
        self.assertEqual([value['action'] for value in messages], ['show', 'cancel'])
        self.assertEqual(messages[0]['rid'], messages[1]['rid'])
        self.assertEqual(self.peer.image_receipts, {})
        self.assertFalse(self.peer.output_lock.locked())

    async def test_only_one_attachment_admitted_and_cancel_releases_slot(self):
        await self.peer.output_lock.acquire()
        first = asyncio.create_task(self.adapter.send_image_file(self.peer.identity, str(self.path)))
        async with asyncio.timeout(1):
            while not self.peer.image_busy:
                await asyncio.sleep(.001)
        second = await self.adapter.send_image_file(self.peer.identity, str(self.path))
        self.assertFalse(second.success)
        self.assertIn('busy', second.error)
        self.assertFalse(self.peer.socket.messages)
        first.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await first
        self.assertFalse(self.peer.image_busy)
        self.peer.output_lock.release()
        self.acknowledge()
        self.assertTrue((await self.adapter.send_image_file(self.peer.identity, str(self.path))).success)

    async def test_slow_file_read_is_bounded_and_releases_admission(self):
        import time
        def slow(*_):
            time.sleep(.1)
            return b'not used'
        with patch.object(screen_image, 'TIMEOUT', .01), patch.object(screen_image, 'owned_image', slow):
            result = await self.adapter.send_image_file(self.peer.identity, str(self.path))
        self.assertFalse(result.success)
        self.assertFalse(self.peer.image_busy)
        self.assertFalse(self.peer.socket.messages)

    async def test_replacement_releases_old_admission_without_crossing_sessions(self):
        await self.peer.output_lock.acquire()
        first = asyncio.create_task(self.adapter.send_image_file(self.peer.identity, str(self.path)))
        async with asyncio.timeout(1):
            while not self.peer.image_busy:
                await asyncio.sleep(.001)
        replacement = Peer(DeviceSocket(), self.peer.identity, {**self.hello, 'screen_image': 1})
        replacement.ready = True
        self.adapter.peers[self.peer.identity] = replacement
        self.peer.output_lock.release()
        result = await first
        self.assertFalse(result.success)
        self.assertFalse(self.peer.image_busy)
        self.assertFalse(replacement.image_busy)
        self.assertFalse(self.peer.socket.messages)
        self.assertFalse(replacement.socket.messages)

    async def test_wrong_receipts_and_close_cannot_confirm_delivery(self):
        rid = 'a' * 32
        future = asyncio.get_running_loop().create_future()
        self.peer.image_receipts[rid] = future
        for result in ({'t': 'screen_image_result', 'rid': rid, 'status': 'shown', 'extra': 1},
                       {'t': 'screen_image_result', 'rid': 'b' * 32, 'status': 'shown'},
                       {'t': 'screen_image_result', 'rid': rid, 'status': 'error'},
                       {'t': 'screen_image_result', 'rid': rid, 'status': 'bogus'}):
            self.assertFalse(screen_image.accept(self.peer, result))
        self.assertFalse(future.done())
        screen_image.close(self.peer)
        self.assertEqual((await future)['status'], 'error')
        self.assertFalse(screen_image.accept(self.peer, {'t': 'screen_image_result', 'rid': rid, 'status': 'shown'}))

    async def test_unsupported_or_failed_caption_never_sends_image(self):
        self.acknowledge()
        self.peer.screen_image_version = None
        self.assertFalse((await self.adapter.send_image_file(self.peer.identity, str(self.path))).success)
        self.peer.screen_image_version = 1
        self.adapter.send = AsyncMock(return_value=SendResult(success=False, error='not shown'))
        self.assertFalse((await self.adapter.send_image_file(self.peer.identity, str(self.path), caption='x')).success)
        self.assertFalse(self.peer.socket.messages)
