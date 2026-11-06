"""Epoch-fenced native playback; cancellation can preempt a pending played receipt."""
import asyncio
import time

from .live_provider import quiet_pcm
from .native_playback import NativePlayback, MAX_QUEUED


class NativeOutput:
    def __init__(self, voice):
        self.voice = voice
        self.stream = self.finishing = self.clearing = None
        self.epoch = 0
        self.queue = asyncio.Queue(maxsize=512)
        self.queued = 0
        self.inflight = 0
        self.tail = bytearray()
        self.last_audio = 0
        self.quiet_task = None
        self.state_lock = asyncio.Lock()
        self.task = asyncio.create_task(self.run())

    async def state(self, value, epoch):
        async with self.state_lock:
            if epoch == self.epoch and self.voice.current():
                await self.voice.peer.send('state', s=value)

    def speaking(self):
        return bool(self.stream and not self.clearing and
                    (self.voice.peer.sent_ms > self.voice.peer.played_ms or
                     time.monotonic() - self.last_audio < .3))

    def audio(self, data):
        pcm, epoch = data.get('pcm'), data.get('epoch')
        if epoch != self.epoch:
            return
        buffered = self.queued + self.inflight + len(self.tail) + (self.stream.queued if self.stream else 0)
        if not isinstance(pcm, bytes) or len(pcm) % 2 or buffered + len(pcm) > MAX_QUEUED:
            raise ValueError('Native output queue limit')
        if not pcm or self.voice.peer.volume < 20:
            return
        self.queued += len(pcm)
        self.queue.put_nowait(('audio', epoch, pcm))

    def clear(self, epoch):
        if type(epoch) is not int or epoch <= self.epoch:
            raise ValueError('Invalid native output epoch')
        self.epoch = epoch
        self.empty_queue()
        self.begin_clear()

    def finish(self):
        self.queue.put_nowait(('finish', self.epoch, None))

    def empty_queue(self):
        while not self.queue.empty():
            self.queue.get_nowait()
        self.queued = 0
        self.tail.clear()

    def stop_quiet_timer(self):
        if self.quiet_task:
            self.quiet_task.cancel()
            self.quiet_task = None

    async def run(self):
        try:
            while True:
                kind, epoch, payload = await self.queue.get()
                if kind == 'audio':
                    self.queued -= len(payload)
                    self.inflight = len(payload)
                    try:
                        await self.accept_audio(payload, epoch)
                    finally:
                        self.inflight = 0
                elif epoch == self.epoch:
                    self.begin_finish(kind == 'finish' and self.voice.mode == 'realtime')
        except asyncio.CancelledError:
            raise
        except Exception:
            await self.voice.fail('voice_playback_failed')

    async def barriers(self):
        if self.finishing:
            try:
                await asyncio.shield(self.finishing)
            except asyncio.CancelledError:
                if asyncio.current_task().cancelling():
                    raise
        if self.clearing:
            await asyncio.shield(self.clearing)
            self.clearing = None

    async def accept_audio(self, pcm, epoch):
        await self.barriers()
        if self.voice.mode == 'realtime':
            await asyncio.wait_for(self.voice.input_stopped.wait(), 3)
        if epoch != self.epoch or not self.voice.current():
            return
        if self.voice.mode == 'live' and quiet_pcm(pcm):
            if self.stream:
                self.tail.extend(pcm)
                if len(self.tail) >= 144000:
                    self.begin_finish(False)
            return
        self.last_audio = time.monotonic()
        if self.stream is None:
            self.stream = NativePlayback(self.voice.peer, self.voice.current)
            self.stream.task.add_done_callback(self.check_task)
            await self.state('speaking', epoch)
        if epoch != self.epoch or not self.voice.current():
            return
        if self.tail:
            self.stream.feed(bytes(self.tail))
            self.tail.clear()
        self.stream.feed(pcm)
        if self.voice.mode == 'live':
            self.stop_quiet_timer()
            self.quiet_task = asyncio.create_task(self.quiet_timeout(epoch))

    def check_task(self, task):
        if not task.cancelled() and task.exception() is not None:
            self.voice.schedule(self.voice.fail('voice_playback_failed'))

    async def quiet_timeout(self, epoch):
        await asyncio.sleep(3)
        self.queue.put_nowait(('quiet', epoch, None))

    def begin_finish(self, final):
        if self.finishing or self.clearing:
            return
        self.tail.clear()
        self.stop_quiet_timer()
        self.finishing = asyncio.create_task(self.finish_stream(self.stream, self.epoch, final))
        self.finishing.add_done_callback(self.check_task)

    async def finish_stream(self, stream, epoch, final):
        try:
            if stream:
                await stream.finish()
            if epoch != self.epoch or not self.voice.current():
                return
            if self.stream is stream:
                self.stream = None
            if final:
                await self.voice.finish()
            else:
                await self.state('listening', epoch)
        finally:
            if self.finishing is asyncio.current_task():
                self.finishing = None

    def begin_clear(self):
        self.stop_quiet_timer()
        if self.clearing:
            return self.clearing
        if self.finishing:
            self.finishing.cancel()
        if self.stream:
            self.stream.task.cancel()
        self.clearing = asyncio.create_task(self.clear_stream(self.stream))
        self.clearing.add_done_callback(self.check_task)
        return self.clearing

    async def clear_stream(self, stream):
        if stream:
            await stream.cancel()
        if self.stream is stream:
            self.stream = None
        if self.voice.current():
            await self.state('listening', self.epoch)

    async def close(self, cancel_stream=True, origin=None):
        self.empty_queue()
        quiet = self.quiet_task
        self.stop_quiet_timer()
        tasks = {task for task in (self.task, self.finishing, quiet)
                 if task and task is not origin and task is not asyncio.current_task()}
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if cancel_stream:
            await asyncio.shield(self.begin_clear())
