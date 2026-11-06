"""GPT-Live public WebSocket protocol with native Hermes client delegation."""
import asyncio
import math
import struct
from uuid import uuid4

from .voice_provider import (MAX_BLOCKS, MAX_TEXT, ProviderError, VoiceProvider,
                             bounded_text, identifier, pcm_decode)


def append_chunks(text):
    """At most 400 UTF-8 bytes: safely below the protocol's 500-token limit."""
    chunk, size = '', 0
    for char in text:
        width = len(char.encode('utf-8'))
        if size + width > 400:
            yield chunk
            chunk, size = '', 0
        chunk += char
        size += width
    if chunk:
        yield chunk


def quiet_pcm(pcm):
    if not pcm:
        return True
    samples = struct.iter_unpack('<h', pcm)
    power = sum(sample[0] ** 2 for sample in samples)
    return math.sqrt(power / (len(pcm) // 2)) < 128


def timeline(event):
    start, end = event.get('start_ms'), event.get('end_ms')
    if not isinstance(start, (int, float)) or not isinstance(end, (int, float)):
        raise ProviderError('invalid_provider_timeline')
    if not math.isfinite(start) or not math.isfinite(end) or start < 0 or end < start:
        raise ProviderError('invalid_provider_timeline')
    return start, end


class LiveProvider(VoiceProvider):
    mode = 'live'
    input_event = 'session.input_audio.append'

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._fragments = []
        self._fragment_chars = 0
        self._delegations = {}
        self._pending = {}
        self._acks = {}
        self._reply_lock = asyncio.Lock()
        self._reply_chars = self._reply_blocks = 0

    async def _configure(self):
        await self.send({'type': 'session.start', 'session': {
            'model': self.settings.model,
            'instructions': self.settings.instructions,
            'audio': {'format': {'type': 'audio/pcm', 'rate': 24000},
                      'output': {'voice': self.settings.voice}},
            'delegation': {'type': 'client'}}})

    async def commit(self):
        # Live listens continuously; there is deliberately no wire commit.
        self.check_open()

    async def reply(self, text, delegation_id=None, final=False):
        self.check_open()
        bounded_text(text)
        state = self._delegations.get(delegation_id)
        if state is None or state['final']:
            raise ProviderError('invalid_provider_delegation')
        self._reply_chars += len(text)
        self._reply_blocks += 1
        try:
            if self._reply_chars > MAX_TEXT or self._reply_blocks > MAX_BLOCKS:
                raise ProviderError('provider_reply_overflow')
            async with self._reply_lock:
                self.check_open()
                # Accepted backend work continues through an interruption. Its
                # results remain context, without requesting the old speech again.
                for chunk in append_chunks(text):
                    await self._append_context(state, chunk, delegation_id)
                if final:
                    state['final'] = True
        finally:
            self._reply_chars -= len(text)
            self._reply_blocks -= 1

    async def _append_context(self, state, text, delegation_id):
        event_id = uuid4().hex
        waiter = asyncio.get_running_loop().create_future()

        def prepare(event):
            # Epoch may change while waiting for an earlier ACK or the writer.
            # Choose the channel at the actual send boundary, never beforehand.
            channel = 'commentary' if state['epoch'] == self.epoch else 'thinking'
            event['type'] = f'session.{channel}.append'
            self._acks[event_id] = (channel, waiter)

        try:
            await self.send({'event_id': event_id, 'delegation_id': delegation_id,
                             'content': text}, before_send=prepare)
            async with asyncio.timeout(self.limits.response):
                await waiter
        except TimeoutError:
            await self.fail('provider_context_timeout')
            raise ProviderError('provider_context_timeout') from None
        finally:
            self._acks.pop(event_id, None)

    async def handle(self, event):
        kind = event['type']
        if kind == 'session.started':
            if not self.ready.is_set():
                self.ready.set()
                await self.emit('ready', {'mode': self.mode})
        elif kind in ('session.input_transcript.delta', 'session.output_transcript.delta'):
            await self._transcript(event)
        elif kind == 'session.delegation.created':
            await self._delegation(event)
        elif kind == 'session.output_audio.delta':
            await self._audio(event)
        elif kind in ('session.commentary.appended', 'session.thinking.appended'):
            pending = self._acks.get(event.get('client_event_id'))
            if pending and kind == f'session.{pending[0]}.appended':
                if not pending[1].done():
                    pending[1].set_result(None)

    async def _transcript(self, event):
        text = bounded_text(event.get('delta'))
        start, end = timeline(event)
        user = event['type'] == 'session.input_transcript.delta'
        data = {'text': text, 'start_ms': start, 'end_ms': end,
                'role': 'user' if user else 'assistant', 'final': False}
        if user:
            self._fragment_chars += len(text)
            if self._fragment_chars > MAX_TEXT or len(self._fragments) >= MAX_BLOCKS:
                raise ProviderError('provider_transcript_overflow')
            self._fragments.append(data)
            if text.strip():
                await self.emit('input_activity', data)
        await self.emit('transcript', data)
        if user:
            await self._dispatch_pending()

    async def _delegation(self, event):
        delegation = event.get('delegation', {})
        if delegation.get('target') != 'client' or delegation.get('type') != 'delegation':
            raise ProviderError('invalid_provider_delegation')
        key = identifier(delegation.get('id'))
        offset = event.get('offset_ms')
        if not isinstance(offset, (int, float)) or not math.isfinite(offset) or offset < 0:
            raise ProviderError('invalid_provider_timeline')
        if key in self._delegations:
            return
        if len(self._delegations) >= MAX_BLOCKS or len(self._pending) >= 32:
            raise ProviderError('provider_delegation_overflow')
        self._delegations[key] = {'epoch': self.epoch, 'final': False}
        timer = asyncio.create_task(self._delegation_expired(key))
        self._pending[key] = (offset, timer)
        await self._dispatch_pending()

    async def _dispatch_pending(self):
        for key, (offset, timer) in list(self._pending.items()):
            available = [part for part in self._fragments if part['start_ms'] <= offset]
            text = ''.join(part['text'] for part in available)
            if not text.strip():
                continue
            del self._pending[key]
            timer.cancel()
            self._fragments = [part for part in self._fragments if part['start_ms'] > offset]
            self._fragment_chars -= len(text)
            await self.emit('delegation', {'id': key, 'text': text, 'offset_ms': offset})

    async def _delegation_expired(self, key):
        await asyncio.sleep(self.limits.connect)
        if key in self._pending:
            await self.fail('provider_delegation_input_missing')

    async def _audio(self, event):
        pcm = pcm_decode(event.get('delta'))
        # Public Live PCM is continuous and must be consumed in delivery order.
        # There is no response boundary or ACK that identifies post-barge audio.
        # Epochs fence our queued playback, not untagged future provider deltas.
        await self.emit('audio', {'pcm': pcm, 'epoch': self.epoch})

    async def interrupt(self, consumed_ms):
        self.check_open()
        if not isinstance(consumed_ms, (int, float)) or not 0 <= consumed_ms <= 120000:
            raise ProviderError('invalid_playback_duration')
        self.epoch += 1
        await self.emit('clear', {'epoch': self.epoch, 'consumed_ms': consumed_ms})
        await self.send({'type': 'session.instructions.append', 'delegation_id': None,
                         'content': 'The caller interrupted. Stop previous speech and listen. '
                         'Playback was cleared; unplayed speech was not heard. '
                         'Accepted backend tasks continue; do not repeat their actions.'})
    async def close(self):
        for _, timer in self._pending.values():
            if timer is not asyncio.current_task():
                timer.cancel()
        for _, waiter in self._acks.values():
            if not waiter.done():
                waiter.cancel()
        await super().close()
