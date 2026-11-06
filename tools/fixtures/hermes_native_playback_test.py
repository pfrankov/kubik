"""Native output respects the firmware's consumed-audio credit and cancel fence."""
import asyncio
import contextlib
import unittest
from unittest.mock import patch

from hermes_test_support import DeviceSocket, KubikAdapter, Peer
from hermes_kubik.native_playback import NativePlayback
from hermes_kubik.codec import decode, encode


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
        KubikAdapter._playback_command(None, self.peer, 'progress',
                                      {'gen': self.peer.gen, 'ms': milliseconds})

    def packets(self):
        return [item for item in self.socket.messages if isinstance(item, bytes)]

    async def test_fills_800ms_prebuffer_and_waits_for_entire_next_frame_credit(self):
        self.playback.feed(bytes(1000 * 48))
        await eventually(lambda: self.peer.sent_ms == 800)
        self.assertEqual(len(self.packets()), 8)
        self.assertTrue(all(len(packet) == 1205 for packet in self.packets()))
        await asyncio.sleep(.02)
        self.assertEqual(self.peer.sent_ms, 800, 'time alone must not grant credit')
        self.progress(99)
        await asyncio.sleep(.02)
        self.assertEqual(self.peer.sent_ms, 800, 'the next 100ms must fit in full')
        self.progress(100)
        await eventually(lambda: self.peer.sent_ms == 900)
        self.assertEqual(self.peer.sent_ms - self.peer.played_ms, 800)
        await asyncio.sleep(.02)
        self.assertEqual(len(self.packets()), 9)

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
            self.assertEqual(len(self.packets()), 9)
            self.assertEqual(len(self.packets()[-1]), 11)
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
        self.assertEqual(len(self.packets()), 8)
        self.assertEqual(self.peer.receipts, {})
        commands = [item['t'] for item in self.socket.messages if isinstance(item, dict)]
        self.assertEqual(commands, ['speak', 'speak_cancel'])

    async def test_duplicate_progress_cannot_renew_a_full_window_stall(self):
        with patch('hermes_kubik.native_playback.STALL_SECONDS', .06):
            self.playback.feed(bytes(1000 * 48))
            await eventually(lambda: self.peer.sent_ms == 800)
            deadline = asyncio.get_running_loop().time() + .2
            while not self.playback.task.done() and asyncio.get_running_loop().time() < deadline:
                self.progress(0)
                await asyncio.sleep(.005)
            self.assertTrue(self.playback.task.done(), 'Repeated receipts must not renew the stall deadline')
            with self.assertRaises(TimeoutError):
                await self.playback.task
            self.assertEqual(self.peer.sent_ms, 800)
            self.assertEqual(self.peer.receipts, {})

    async def test_only_forward_consumption_renews_stall_deadline_and_grants_credit(self):
        with patch('hermes_kubik.native_playback.STALL_SECONDS', .15):
            self.playback.feed(bytes(1000 * 48))
            await eventually(lambda: self.peer.sent_ms == 800)
            await asyncio.sleep(.08)
            self.progress(40)
            await asyncio.sleep(.08)
            self.assertFalse(self.playback.task.done(), 'Real consumption must renew the deadline')
            self.progress(39)
            self.progress(40)
            await asyncio.sleep(.02)
            self.assertEqual(self.peer.played_ms, 40, 'Backward receipts cannot change consumption')
            self.assertEqual(self.peer.sent_ms, 800, 'Partial progress cannot overfill the window')
            self.progress(100)
            await eventually(lambda: self.peer.sent_ms == 900)
            self.assertEqual(self.peer.sent_ms - self.peer.played_ms, 800)

    async def test_short_live_reply_flushes_within_100ms_without_finish(self):
        self.playback.feed(bytes(20 * 48))
        await asyncio.sleep(.04)
        self.assertFalse(self.packets())
        self.playback.feed(bytes(20 * 48))
        await asyncio.sleep(.04)
        self.assertFalse(self.packets())
        self.playback.feed(bytes(20 * 48))
        async with asyncio.timeout(.1):
            await eventually(lambda: bool(self.packets()))
        self.assertEqual(self.peer.sent_ms, 60)
        self.assertEqual(len(self.packets()[0]), 725)
        self.assertFalse(self.playback.ended)

    async def test_finished_short_reply_flushes_exact_tail_immediately(self):
        self.playback.feed(bytes(40 * 48 + 22))
        await asyncio.wait_for(self.playback.finish(), .1)
        self.assertEqual(len(self.packets()), 1)
        self.assertEqual(self.playback.sent_bytes, 40 * 48 + 24)
        self.assertEqual(len(decode(self.packets()[0][2:], max_samples=2400)), 40 * 48 + 24)
        self.assertEqual(self.playback.queued, 0)

    async def test_output_decoder_bound_does_not_expand_microphone_frames(self):
        output = encode(bytes(100 * 48), [0, 0])
        self.assertEqual(len(decode(output, max_samples=2400)), 100 * 48)
        with self.assertRaises(ValueError):
            decode(output)
        with self.assertRaises(ValueError):
            decode(encode(bytes(40 * 48 + 4), [0, 0]))
        with self.assertRaises(ValueError):
            decode(output + b'\0', max_samples=2400)
