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
