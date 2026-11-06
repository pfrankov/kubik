"""Native output respects the firmware's consumed-audio credit and cancel fence."""
import asyncio
import contextlib
import unittest

from hermes_test_support import DeviceSocket, Peer
from hermes_kubik.native_playback import NativePlayback


async def eventually(predicate):
    async with asyncio.timeout(2):
        while not predicate():
            await asyncio.sleep(.001)


class NativePlaybackCredit(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.socket = DeviceSocket()
        self.peer = Peer(self.socket, 'test', {'volume': 50})
        self.playback = NativePlayback(self.peer, lambda: True)
        original = self.socket.send_json

        async def acknowledge(value):
            await original(value)
            if value['t'] == 'speak_cancel':
                self.playback.cancelled.set()
            elif value['t'] == 'speak_end':
                self.peer.receipts[('played', value['gen'])].set()

        self.socket.send_json = acknowledge

    async def asyncTearDown(self):
        self.playback.task.cancel()
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await self.playback.task

    def progress(self, milliseconds):
        self.peer.played_ms = milliseconds
        self.peer.progress.set()

    def packets(self):
        return [item for item in self.socket.messages if isinstance(item, bytes)]

    async def test_fills_800ms_prebuffer_and_waits_for_entire_next_frame_credit(self):
        self.playback.feed(bytes(1000 * 48))
        await eventually(lambda: self.peer.sent_ms == 800)
        self.assertEqual(len(self.packets()), 20)
        await asyncio.sleep(.02)
        self.assertEqual(self.peer.sent_ms, 800, 'time alone must not grant credit')
        self.progress(39)
        await asyncio.sleep(.02)
        self.assertEqual(self.peer.sent_ms, 800, 'the next 40ms must fit in full')
        self.progress(40)
        await eventually(lambda: self.peer.sent_ms == 840)
        self.assertEqual(self.peer.sent_ms - self.peer.played_ms, 800)
        await asyncio.sleep(.02)
        self.assertEqual(len(self.packets()), 21)

    async def test_partial_tail_obeys_byte_accurate_credit_then_finishes(self):
        self.playback.feed(bytes(800 * 48 + 24))
        finish = asyncio.create_task(self.playback.finish())
        try:
            await eventually(lambda: self.peer.sent_ms == 800)
            await asyncio.sleep(.02)
            self.assertFalse(finish.done(), 'half a millisecond still needs credit')
            self.assertEqual(self.playback.sent_bytes, 800 * 48)
            self.progress(1)
            await asyncio.wait_for(finish, 2)
            self.assertEqual(self.playback.sent_bytes, 800 * 48 + 24)
            self.assertEqual(len(self.packets()), 21)
            self.assertEqual(self.peer.receipts, {})
        finally:
            finish.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await finish

    async def test_cancel_preempts_full_window_without_sending_queued_pcm(self):
        self.playback.feed(bytes(5000 * 48))
        await eventually(lambda: self.peer.sent_ms == 800)
        await asyncio.wait_for(self.playback.cancel(), .2)
        self.progress(800)
        await asyncio.sleep(.02)
        self.assertEqual(len(self.packets()), 20)
        self.assertEqual(self.peer.receipts, {})
        commands = [item['t'] for item in self.socket.messages if isinstance(item, dict)]
        self.assertEqual(commands, ['speak', 'speak_cancel'])
