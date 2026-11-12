"""Color previews preserve the image and do not alter the print raster."""
import asyncio
from io import BytesIO
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from PIL import Image
import hermes_test_support
from hermes_kubik.printer_image_raster import prepare
from hermes_kubik.printer_preview import prepare_with_preview
from hermes_kubik.printer_image_stream import abort, preview, transmit


class PreviewTest(unittest.IsolatedAsyncioTestCase):
    def test_color_fit_and_print_bytes_are_unchanged(self):
        image = Image.new('RGB', (100, 50), 'red')
        data = BytesIO(); image.save(data, format='PNG')
        geometry = dict(head_width=576, width_dots=304, height_limit=1000, dpi=304, feed_dpi=225)
        result = prepare_with_preview(data.getvalue(), **geometry)
        self.assertEqual(result.data, prepare(data.getvalue(), **geometry).data)
        self.assertEqual(len(result.preview), 8224)
        palette, pixels = result.preview[:32], result.preview[32:]
        def color(x, y):
            offset = y * 128 + x
            code = pixels[offset // 2]
            index = code >> 4 if offset % 2 == 0 else code & 15
            return int.from_bytes(palette[2 * index:2 * index + 2], 'big')
        self.assertEqual(color(64, 0), 0)
        self.assertEqual(color(64, 64), 0xf800)
        self.assertEqual(color(64, 127), 0)

    def test_preview_memory_failure_keeps_printable_raster(self):
        data = BytesIO(); Image.new('RGB', (20, 20), 'red').save(data, format='PNG')
        geometry = dict(head_width=576, width_dots=304, height_limit=1000, dpi=304, feed_dpi=225)
        with patch('hermes_kubik.printer_preview.make_preview', side_effect=MemoryError):
            result = prepare_with_preview(data.getvalue(), **geometry)
        self.assertIsNone(result.preview)
        self.assertEqual(result.data, prepare(data.getvalue(), **geometry).data)

    async def test_frames_are_ordered_and_only_for_supported_standalone_image(self):
        class Socket:
            def __init__(self): self.frames = []
            async def send_bytes(self, data): self.frames.append(data)
        socket = Socket()
        peer = SimpleNamespace(socket=socket, printer_preview_version=1)
        data = bytes(range(256)) * 32 + bytes(range(32))
        row = {'peer': peer, 'raster': SimpleNamespace(preview=data), 'rid': 'a' * 32,
               'parameters': {'format': 'image'}}
        await preview(row)
        self.assertEqual(len(socket.frames), 10)
        self.assertEqual(b''.join(frame[36:] for frame in socket.frames), data)
        self.assertEqual([int.from_bytes(frame[34:36], 'big') for frame in socket.frames], list(range(0, 8224, 896)))
        self.assertTrue(all(frame[:34] == b'\x07\x00' + b'a' * 32 for frame in socket.frames))
        socket.frames.clear(); row['parameters']['format'] = 'document'
        await preview(row)
        self.assertFalse(socket.frames)
        row['parameters']['format'] = 'image'; peer.printer_preview_version = None
        await preview(row)
        self.assertFalse(socket.frames)


    async def test_cancel_during_preview_never_starts_print(self):
        entered, resume = asyncio.Event(), asyncio.Event()
        class Socket:
            async def send_bytes(self, data):
                entered.set()
                await resume.wait()
        class Peer:
            printer_preview_version = 1
            socket = Socket()
            def __init__(self): self.starts = []
            async def send(self, kind, **fields): self.starts.append((kind, fields))
        peer = Peer()
        row = {'peer': peer, 'raster': SimpleNamespace(preview=bytes(8224), head_width=576, width=10, height=10),
               'rid': 'a' * 32, 'parameters': {'format': 'image'},
               'quiescent': asyncio.get_running_loop().create_future()}
        task = asyncio.create_task(transmit(row))
        await entered.wait()
        row['aborting'] = True
        resume.set()
        await task
        self.assertFalse(peer.starts)

    async def test_abort_never_overtakes_an_in_flight_start(self):
        entered, resume = asyncio.Event(), asyncio.Event()
        class Peer:
            printer_preview_version = None
            def __init__(self): self.actions = []
            async def send(self, kind, **fields):
                if fields['action'] == 'start':
                    entered.set()
                    await resume.wait()
                self.actions.append(fields['action'])
        peer = Peer()
        row = {'peer': peer, 'raster': SimpleNamespace(preview=None, head_width=576, width=10, height=10),
               'rid': 'a' * 32, 'parameters': {'format': 'image'},
               'quiescent': asyncio.get_running_loop().create_future()}
        task = asyncio.create_task(transmit(row))
        await entered.wait()
        row['aborting'] = True
        stopping = asyncio.create_task(abort(row))
        await asyncio.sleep(0)
        resume.set()
        await asyncio.gather(task, stopping)
        self.assertEqual(peer.actions, ['start', 'abort'])

    async def test_failed_preview_does_not_raise(self):
        class Broken:
            async def send_bytes(self, data): raise ConnectionError()
        row = {'peer': SimpleNamespace(socket=Broken(), printer_preview_version=1),
               'raster': SimpleNamespace(preview=bytes(8224)), 'rid': 'a' * 32,
               'parameters': {'format': 'image'}}
        await preview(row)
