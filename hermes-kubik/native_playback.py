"""One bounded native PCM stream, paced by actual device playback receipts."""
import asyncio
import contextlib

from .codec import encode

MAX_QUEUED = 60 * 24000 * 2
SPEECH_LEAD_MS = 800  # Fill the firmware prebuffer; retain 100 ms ring headroom.


class NativePlayback:
    def __init__(self, peer, current):
        self.peer, self.current = peer, current
        self.queue = asyncio.Queue(maxsize=512)
        self.queued = 0
        self.gen = None
        self.sent_bytes = 0
        self.ended = False
        self.cancelled = asyncio.Event()
        self.task = asyncio.create_task(self.run())

    def feed(self, pcm):
        if self.ended or not self.current():
            return
        if not isinstance(pcm, bytes) or not pcm or len(pcm) % 2 or self.queued + len(pcm) > MAX_QUEUED:
            raise ValueError('Native audio queue limit')
        self.queue.put_nowait(pcm)
        self.queued += len(pcm)

    async def finish(self):
        if not self.ended:
            self.ended = True
            self.queue.put_nowait(None)
        await self.task

    async def cancel(self):
        self.ended = True
        self.task.cancel()
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await self.task
        if self.gen is not None and not self.peer.socket.closed:
            await self.peer.send('speak_cancel', gen=self.gen)
            await asyncio.wait_for(self.cancelled.wait(), 3)

    async def run(self):
        async with self.peer.output_lock:
            async with asyncio.timeout(140):
                await self.stream()

    async def stream(self):
        peer = self.peer
        pending = bytearray()
        state = [0, 0]
        done = asyncio.Event()
        try:
            while True:
                block = await self.queue.get()
                if block is None:
                    break
                pending.extend(block)
                while len(pending) >= 1920:
                    await self.send_block(bytes(pending[:1920]), state, done)
                    del pending[:1920]
                    self.queued -= 1920
            if pending:
                self.queued -= len(pending)
                if len(pending) % 4:
                    pending.extend(b'\0\0')
                await self.send_block(bytes(pending), state, done)
            if self.gen is not None:
                await peer.send('speak_end', gen=self.gen)
                await asyncio.wait_for(done.wait(), 12)
                if not self.current():
                    raise ConnectionError('Native voice disconnected')
        finally:
            if self.gen is not None:
                peer.receipts.pop(('played', self.gen), None)

    async def send_block(self, block, state, done):
        if not self.current():
            raise ConnectionError('Native voice disconnected')
        peer = self.peer
        if self.sent_bytes + len(block) > 120 * 48000:
            raise ValueError('Native response exceeds 120 seconds')
        if self.gen is None:
            peer.gen = peer.gen % 255 + 1
            self.gen = peer.gen
            peer.played_ms = peer.sent_ms = 0
            peer.receipts[('played', self.gen)] = done
            await peer.send('speak', gen=self.gen, kind='reply')
        while self.sent_bytes + len(block) > (peer.played_ms + SPEECH_LEAD_MS) * 48:
            peer.progress.clear()
            await asyncio.wait_for(peer.progress.wait(), 5)
            if not self.current():
                raise ConnectionError('Native voice disconnected')
        await peer.socket.send_bytes(bytes((3, self.gen)) + encode(block, state))
        self.sent_bytes += len(block)
        peer.sent_ms = self.sent_bytes // 48
