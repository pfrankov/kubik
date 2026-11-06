"""Device-bound Realtime and continuous Live, using the native Hermes agent pipeline."""
import asyncio
import contextlib

from .codec import decode
from .echo import EchoCleaner
from .native_output import NativeOutput
from .voice_provider import ProviderSettings, create_provider
from .voice_settings import native_settings


class NativeVoice:
    def __init__(self, adapter, peer, mode, turn):
        self.adapter, self.peer, self.mode, self.turn = adapter, peer, mode, turn
        self.provider = self.echo = self.output = None
        self.closed = False
        self.cleanup_task = None
        self.tasks = set()
        self.input_queue = asyncio.Queue(maxsize=125)
        self.input_ack = None
        self.input_stopped = asyncio.Event()
        self.pause_task = None
        self.input_ended = False
        self.committed = False
        self.transcript_seen = False
        self.interrupting = False
        self.text_pending = ''
        self.text_task = None

    @classmethod
    async def ptt(cls, adapter, peer, turn, on):
        voice = peer.native
        if voice:
            if turn == voice.turn and not on and voice.mode == 'realtime':
                voice.commit()
            elif on:
                await peer.send('error', code='busy')
            return
        if not on:
            return
        if peer.turn is not None or adapter.native_jobs.busy(peer.identity) or (peer.task and not peer.task.done()):
            await peer.send('error', code='busy')
            return
        mode = adapter.voice_controls.mode(peer)
        voice = peer.native = cls(adapter, peer, mode, turn)
        peer.turn = peer.input_turn = turn
        peer.stage = 'native'
        voice.schedule(voice.start())

    def current(self):
        return (not self.closed and self.peer.native is self and self.peer.ready and
                self.adapter._ready_peer(self.peer.identity) is self.peer)

    def schedule(self, coroutine):
        task = asyncio.create_task(self.guard(coroutine))
        self.tasks.add(task)
        task.add_done_callback(self.tasks.discard)
        return task

    async def guard(self, coroutine):
        try:
            await coroutine
        except asyncio.CancelledError:
            raise
        except Exception:
            await self.fail('voice_failed')

    async def start(self):
        choices = self.adapter.voice_controls.choices(self.peer)
        settings = await asyncio.to_thread(native_settings, self.adapter.home, self.mode, choices.get(self.mode))
        if not self.current():
            return
        if self.mode == 'live':
            self.echo = EchoCleaner()
        self.output = NativeOutput(self)
        self.provider = create_provider(self.mode, ProviderSettings(**settings), self.event)
        await self.provider.start()
        if not self.current():
            return
        if self.mode == 'live':
            await self.input_control(True)
        await self.peer.send('state', s='listening')
        self.schedule(self.input_worker())

    def microphone(self, frame):
        if not self.current():
            raise ValueError('Native microphone authorization changed')
        if len(frame) < 2 or frame[1] != self.turn:
            raise ValueError('Invalid native microphone turn')
        if self.input_stopped.is_set():
            return
        if self.mode == 'live':
            if len(frame) != 968 or frame[0] != 5:
                raise ValueError('Live microphone requires paired speaker reference')
            pcm, reference = decode(frame[2:485]), decode(frame[485:])
        else:
            if not 6 <= len(frame) <= 485 or frame[0] != 4:
                raise ValueError('Invalid Realtime microphone frame')
            pcm, reference = decode(frame[2:]), None
        self.input_queue.put_nowait((pcm, reference))

    async def input_worker(self):
        while self.current():
            value = await self.input_queue.get()
            if value is None:
                await self.provider.commit()
                continue
            pcm, reference = value
            if self.echo:
                pcm = self.echo.process(pcm, reference)
            await self.provider.append(pcm)

    def commit(self):
        if not self.committed:
            self.committed = True
            self.input_queue.put_nowait(None)

    async def input_control(self, on):
        if not self.current():
            raise ConnectionError('Native conversation closed')
        done = asyncio.Event()
        self.input_ack = (on, done)
        try:
            await self.peer.send('live_input', turn=self.turn, on=on)
            await asyncio.wait_for(done.wait(), 3)
            if not self.current():
                raise ConnectionError('Native conversation closed')
        finally:
            self.input_ack = None

    def acknowledge(self, kind, value):
        if kind == 'live_input_ack':
            wait = self.input_ack
            if wait and value.get('turn') == self.turn and value.get('on') is wait[0]:
                wait[1].set()
        elif kind == 'cancelled' and self.output and self.output.stream:
            stream = self.output.stream
            if value.get('gen') == stream.gen:
                stream.cancelled.set()

    def pause(self):
        if self.pause_task is None:
            self.pause_task = self.schedule(self.stop_input())
        return self.pause_task

    async def stop_input(self):
        await self.input_control(False)
        self.input_stopped.set()
        await self.end_input()
        if self.current():
            await self.peer.send('state', s='thinking')

    async def end_input(self):
        if not self.input_ended and not self.peer.socket.closed:
            self.input_ended = True
            await self.peer.send('input_end', turn=self.turn, session=self.peer.session_id)
        if self.peer.turn == self.turn:
            self.peer.turn = None

    async def event(self, kind, data):
        if not self.current():
            if not self.closed:
                self.schedule(self.close())
            return
        if kind == 'audio':
            self.output.audio(data)
        elif kind == 'input_end':
            self.pause()
        elif kind == 'transcript':
            self.transcript(data)
        elif kind == 'delegation':
            self.adapter.native_jobs.submit(self, data['text'], data['id'])
        elif kind == 'response_done' and data.get('final'):
            self.output.finish()
        elif kind == 'clear':
            self.output.clear(data['epoch'])
        elif kind == 'input_activity':
            await self.interrupt()
        elif kind == 'error':
            self.schedule(self.fail('voice_failed'))

    def transcript(self, data):
        if self.mode == 'realtime' and data.get('role') == 'user' and data.get('final'):
            if self.transcript_seen:
                return
            self.transcript_seen = True
            self.pause()
            self.schedule(self.dispatch_realtime(data['text']))
        elif self.mode == 'live' and data.get('role') == 'assistant' and self.peer.volume < 20:
            self.text_pending += data['text']
            if len(self.text_pending) > 8192:
                raise ValueError('Native text queue limit')
            if self.text_task is None or self.text_task.done():
                self.text_task = self.schedule(self.show_text())

    async def show_text(self):
        while self.text_pending and self.current():
            await asyncio.sleep(.4)
            text = self.text_pending.encode('utf-8')[:1000].decode('utf-8', errors='ignore')
            self.text_pending = self.text_pending[len(text):]
            result = await self.adapter._send_text(self.peer, text)
            if not result.success:
                raise ConnectionError('Native text not acknowledged')

    async def dispatch_realtime(self, text):
        await self.pause()
        if self.current():
            self.adapter.native_jobs.submit(self, text)

    async def interrupt(self):
        if self.mode == 'live' and self.output.speaking() and not self.interrupting:
            self.interrupting = True
            # Epoch changes before the reader accepts a following delegation.
            # Provider interrupt writes instructions; it never waits for a reader ACK.
            await self.interrupt_output()

    async def interrupt_output(self):
        try:
            await self.provider.interrupt(self.peer.played_ms)
        finally:
            self.interrupting = False

    async def reply(self, text, delegation_id=None, final=False):
        if not self.current():
            return
        if self.mode == 'realtime':
            await self.pause()
            if text and self.peer.volume < 20:
                result = await self.adapter._send_text(self.peer, text)
                if not result.success:
                    raise ConnectionError('Native text not acknowledged')
        if self.current():
            await self.provider.reply(text, delegation_id, final)

    async def cancel_request(self, value):
        if 'turn' in value and value['turn'] != self.turn:
            return
        if 'gen' in value and value['gen'] != self.peer.gen:
            return
        self.schedule(self.close())

    async def finish(self):
        await self.close(cancel_output=False)

    async def fail(self, code):
        if self.current():
            await self.peer.send('error', code=code)
        await self.close()

    async def close(self, cancel_output=True):
        if self.cleanup_task is None:
            self.closed = True
            self.cleanup_task = asyncio.create_task(self.shutdown(cancel_output, asyncio.current_task()))
        await asyncio.shield(self.cleanup_task)

    async def stop_tasks(self, origin):
        pending = []
        for task in tuple(self.tasks):
            if task is not origin:
                task.cancel()
                pending.append(task)
        await asyncio.gather(*pending, return_exceptions=True)

    async def shutdown(self, cancel_output, origin):
        try:
            with contextlib.suppress(ConnectionError, OSError):
                await self.end_input()
            await self.stop_tasks(origin)
            if self.output:
                try:
                    await self.output.close(cancel_output, origin)
                except (TimeoutError, ConnectionError):
                    # An unacknowledged generation must not overlap a later conversation.
                    await self.peer.socket.close(code=4001)
            if self.provider:
                await self.provider.close()
        finally:
            if self.echo:
                self.echo.close()
            if self.peer.native is self:
                self.peer.native = None
                self.peer.input_turn = None
                self.peer.stage = 'idle'
                if not self.peer.socket.closed:
                    await self.peer.send('state', s='idle')
