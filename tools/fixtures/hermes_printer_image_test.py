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


class ImageOriginTest(ProtocolFixture):
    async def test_only_generated_local_png_is_snapshotted_and_turn_scoped(self):
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

    async def asyncTearDown(self):
        await self.jobs.shutdown()
        await super().asyncTearDown()

    async def wait_for(self, predicate):
        async with asyncio.timeout(1):
            while not predicate():
                await asyncio.sleep(.001)

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

    async def test_timeout_aborts_without_replay_and_keeps_output_lease(self):
        task = asyncio.create_task(self.jobs.request(self.peer, 'call', self.args, 'event', self.raster))
        value = await task
        self.assertEqual((value['code'], value['outcome']), ('timeout', 'unknown'))
        self.assertEqual(self.peer.socket.messages[-1]['action'], 'abort')
        self.assertTrue(self.peer.output_lock.locked())
        self.assertEqual(await self.jobs.request(self.peer, 'call', self.args, 'event', self.raster), value)
        self.assertEqual(len(self.peer.socket.messages), 2)
