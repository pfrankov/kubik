"""One bounded native PCM stream, paced by actual device playback receipts."""
import asyncio
import contextlib

from .codec import encode

MAX_QUEUED = 60 * 24000 * 2
SPEECH_LEAD_MS = 800  # Fill the firmware prebuffer; retain 100 ms ring headroom.
PACKET_BYTES = 100 * 48
STALL_SECONDS = 5


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
        state = [0, 0]
        done = asyncio.Event()
        try:
            async for block in self.frames():
                await self.send_block(block, state, done)
            if self.gen is not None:
                await peer.send('speak_end', gen=self.gen)
                await asyncio.wait_for(done.wait(), 12)
                if not self.current():
                    raise ConnectionError('Native voice disconnected')
        finally:
            if self.gen is not None:
                peer.receipts.pop(('played', self.gen), None)

    async def next_input(self, deadline):
        try:
            async with asyncio.timeout_at(deadline):
                return await self.queue.get()
        except TimeoutError:
            return b''

    def take_frame(self, pending):
        pcm = bytes(pending)
        pending.clear()
        self.queued -= len(pcm)
        return pcm + b'\0\0' if len(pcm) % 4 else pcm

    async def frames(self):
        pending, deadline = bytearray(), None
        while True:
            block = await self.next_input(deadline)
            if block is None:
                break
            view = memoryview(block)
            while view:
                size = min(PACKET_BYTES - len(pending), len(view))
                pending.extend(view[:size])
                view = view[size:]
                if len(pending) == PACKET_BYTES:
                    yield self.take_frame(pending)
                    deadline = None
            if pending and deadline is None:
                deadline = asyncio.get_running_loop().time() + .1
            if not block and pending:
                yield self.take_frame(pending)
                deadline = None
        if pending:
            yield self.take_frame(pending)

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
        await self.wait_for_credit(len(block))
        await peer.socket.send_bytes(bytes((3, self.gen)) + encode(block, state))
        self.sent_bytes += len(block)
        peer.sent_ms = self.sent_bytes // 48

    async def wait_for_credit(self, size):
        peer, loop = self.peer, asyncio.get_running_loop()
        confirmed = peer.played_ms
        deadline = loop.time() + STALL_SECONDS
        while self.sent_bytes + size > (peer.played_ms + SPEECH_LEAD_MS) * 48:
            peer.progress.clear()
            async with asyncio.timeout_at(deadline):
                await peer.progress.wait()
            if not self.current():
                raise ConnectionError('Native voice disconnected')
            if peer.played_ms > confirmed:
                confirmed = peer.played_ms
                deadline = loop.time() + STALL_SECONDS
