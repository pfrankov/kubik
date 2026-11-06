"""Native voice device lifecycle with real codec/AEC and controlled provider events."""
import asyncio
import contextlib
from pathlib import Path
import struct
from types import SimpleNamespace
from unittest.mock import patch

from hermes_test_support import (DeviceSocket, KubikAdapter, Peer, ProtocolFixture,
                                atomic_json, encode)
from hermes_kubik.echo import EchoCleaner
from hermes_kubik.voice_settings import VoicePreferences

ROOT = Path(__file__).resolve().parents[2]
PCM = struct.pack('<960h', *([500, -500] * 480))
SETTINGS = dict(api_key='fixture', base_url='https://api.openai.test/v1',
                model='gpt-realtime-2.1', voice='marin', instructions='')


async def eventually(predicate):
    async with asyncio.timeout(2):
        while not predicate():
            await asyncio.sleep(.001)


class Provider:
    def __init__(self, mode, settings, callback):
        self.mode, self.callback = mode, callback
        self.started = self.closed = False
        self.input, self.replies = [], []
        self.commits = self.epoch = 0

    async def start(self):
        self.started = True
        await self.callback('ready', {'mode': self.mode})

    async def append(self, pcm):
        self.input.append(pcm)

    async def commit(self):
        self.commits += 1

    async def reply(self, text, delegation_id=None, final=False):
        self.replies.append((text, delegation_id, final))
        if text:
            await self.callback('audio', {'pcm': PCM, 'epoch': self.epoch})
        if final and self.mode == 'realtime':
            await self.callback('response_done', {'final': True})

    async def interrupt(self, consumed_ms):
        self.epoch += 1
        await self.callback('clear', {'epoch': self.epoch, 'consumed_ms': consumed_ms})

    async def close(self):
        self.closed = True


class NativeVoiceContract(ProtocolFixture):
    def setUp(self):
        super().setUp()
        self.adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
        self.adapter.security = self.store
        self.adapter.speech_available = lambda: ({'available': True}, {'available': True})
        self.identity, _ = self.store.identity(self.hello)
        atomic_json(self.store.allow_path, [self.identity])
        self.socket = DeviceSocket()
        self.peer = Peer(self.socket, self.identity, self.hello)
        self.peer.ready = True
        self.adapter.peers[self.identity] = self.peer
        self.providers = []
        self.auto_ack = True
        original_json = self.socket.send_json
        async def acknowledge(value):
            await original_json(value)
            if not self.auto_ack:
                return
            if value['t'] == 'live_input':
                await self.adapter.command(self.peer, {**value, 't': 'live_input_ack'})
            elif value['t'] == 'speak_cancel':
                await self.adapter.command(self.peer, {**value, 't': 'cancelled'})
            elif value['t'] == 'speak_end':
                await self.adapter.command(self.peer, {'t': 'played', 'gen': value['gen'],
                                                       'ms': int(self.peer.sent_ms)})
            elif value['t'] == 'text':
                await self.adapter.command(self.peer, {'t': 'shown', 'receipt': value['receipt']})
        self.socket.send_json = acknowledge
        def create(mode, settings, callback):
            provider = Provider(mode, settings, callback)
            self.providers.append(provider)
            return provider
        for patcher in [patch('hermes_kubik.native_voice.native_settings', return_value=SETTINGS),
                        patch('hermes_kubik.native_voice.create_provider', side_effect=create),
                        patch('hermes_kubik.native_voice.EchoCleaner', side_effect=lambda: EchoCleaner(
                            ROOT / 'openclaw-kubik/src/vendor/speex-aec.wasm'))]:
            patcher.start()
            self.addCleanup(patcher.stop)
        self.addAsyncCleanup(self.cleanup_voice)

    async def cleanup_voice(self):
        await self.socket.close()
        if self.peer.native:
            await self.peer.native.close()
        for job in tuple(self.adapter.native_jobs.jobs.values()):
            job.complete.set()
            await job.task

    async def start(self, mode):
        VoicePreferences(self.store.home).select(self.identity, 'mode', mode)
        await self.adapter.ptt(self.peer, {'turn': 7, 'on': True})
        await eventually(lambda: bool(self.providers) and self.providers[0].started)
        await eventually(lambda: any(isinstance(item, dict) and item.get('s') == 'listening'
                                    for item in self.socket.messages))
        return self.peer.native, self.providers[0]

    async def test_realtime_stops_mic_before_agent_audio_and_uses_native_history_path(self):
        seen = []
        async def handle(event):
            self.assertTrue(event.source.role_authorized)
            self.assertFalse(self.adapter._should_auto_tts_for_chat(self.identity))
            seen.append(event.text)
            event._gateway_accepted = True
            async def background():
                await self.adapter.send(self.identity, 'Native Hermes answer', reply_to=event.message_id)
                await self.adapter.on_processing_complete(event, 'success')
            asyncio.create_task(background())
        self.adapter.handle_message = handle
        voice, provider = await self.start('realtime')
        self.adapter.microphone(self.peer, bytes((4, 7)) + encode(PCM, [0, 0]))
        await eventually(lambda: bool(provider.input))
        await provider.callback('input_end', {'item_id': 'input'})
        await provider.callback('transcript', {'text': 'hello', 'role': 'user', 'final': True})
        await eventually(lambda: self.peer.native is None)
        self.assertEqual(seen, ['hello'])
        self.assertEqual(provider.replies, [('Native Hermes answer', None, False), ('', None, True)])
        kinds = [value['t'] for value in self.socket.messages if isinstance(value, dict)]
        self.assertLess(kinds.index('live_input'), kinds.index('input_end'))
        self.assertLess(kinds.index('input_end'), kinds.index('speak'))
        self.assertIn('speak_end', kinds)
        self.assertTrue(provider.closed)
        self.assertFalse(self.adapter.native_jobs.jobs)

    async def test_live_requires_paired_reference_and_keeps_input_during_output(self):
        voice, provider = await self.start('live')
        paired = bytes((5, 7)) + encode(PCM, [0, 0]) + encode(bytes(1920), [0, 0])
        self.adapter.microphone(self.peer, paired)
        await eventually(lambda: bool(provider.input))
        self.assertEqual(len(provider.input[0]), 1920)
        await provider.callback('audio', {'pcm': PCM, 'epoch': 0})
        await eventually(lambda: voice.output.stream is not None and voice.output.stream.gen is not None)
        self.adapter.microphone(self.peer, paired)
        await eventually(lambda: len(provider.input) == 2)
        await provider.callback('input_activity', {'text': 'interrupt', 'start_ms': 20, 'end_ms': 40})
        await eventually(lambda: provider.epoch == 1 and voice.output.stream is None)
        self.assertIs(self.peer.native, voice)
        self.assertFalse(provider.closed)
        self.assertTrue(any(isinstance(item, dict) and item['t'] == 'speak_cancel'
                            for item in self.socket.messages))
        with self.assertRaises(ValueError):
            self.adapter.microphone(self.peer, bytes((4, 7)) + encode(PCM, [0, 0]))
        await self.adapter.cancel_request(self.peer, {'turn': 7})
        await eventually(lambda: self.peer.native is None)
        self.assertTrue(provider.closed)

    async def test_cancelled_close_caller_does_not_abandon_output_cleanup(self):
        voice, provider = await self.start('live')
        entered, release = asyncio.Event(), asyncio.Event()
        async def close_provider():
            entered.set()
            await release.wait()
            provider.closed = True
        provider.close = close_provider
        closing = asyncio.create_task(voice.close())
        await entered.wait()
        closing.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await closing
        joined = asyncio.create_task(voice.close())
        await asyncio.sleep(.01)
        try:
            self.assertFalse(joined.done(), 'later close must join the original cleanup')
        finally:
            release.set()
            await joined
        self.assertTrue(provider.closed)
        self.assertTrue(voice.output.task.done())
        self.assertIsNone(self.peer.native)

    async def test_final_realtime_cleanup_owns_output_until_provider_close_finishes(self):
        voice, provider = await self.start('realtime')
        entered, release = asyncio.Event(), asyncio.Event()
        async def close_provider():
            entered.set()
            await release.wait()
            provider.closed = True
        provider.close = close_provider
        await provider.callback('response_done', {'final': True})
        await entered.wait()
        joined = asyncio.create_task(voice.close())
        await asyncio.sleep(.01)
        try:
            self.assertFalse(joined.done())
            self.assertTrue(voice.output.task.done(), 'final output consumer must stop before provider shutdown')
        finally:
            release.set()
            await joined
        self.assertTrue(provider.closed)
        self.assertIsNone(self.peer.native)

    async def test_disconnect_detaches_delivery_without_cancelling_accepted_hermes_work(self):
        accepted, release, completed = asyncio.Event(), asyncio.Event(), asyncio.Event()
        async def handle(event):
            event._gateway_accepted = True
            async def background():
                accepted.set()
                await release.wait()
                result = await self.adapter.send(self.identity, 'late result')
                self.assertTrue(result.success)
                await self.adapter.on_processing_complete(event, 'success')
                completed.set()
            asyncio.create_task(background())
        self.adapter.handle_message = handle
        voice, provider = await self.start('live')
        await provider.callback('delegation', {'id': 'delegation', 'text': 'run a tool'})
        await accepted.wait()
        await self.socket.close()
        await self.adapter._remove_peer(self.socket, self.peer)
        self.assertTrue(self.adapter.native_jobs.busy(self.identity))
        replacement = Peer(DeviceSocket(), self.identity, self.hello)
        replacement.ready = True
        self.adapter.peers[self.identity] = replacement
        release.set()
        await asyncio.wait_for(completed.wait(), 2)
        await eventually(lambda: not self.adapter.native_jobs.busy(self.identity))
        self.assertEqual(replacement.socket.messages, [])
        self.assertEqual(provider.replies, [])

    async def test_revocation_blocks_frames_and_late_native_delivery(self):
        voice, provider = await self.start('live')
        atomic_json(self.store.allow_path, [])
        with self.assertRaises(ValueError):
            voice.microphone(bytes((5, 7)) + bytes(966))
        await voice.reply('revoked reply', 'delegation')
        self.assertEqual(provider.replies, [])

    async def test_realtime_output_waits_for_matching_input_ack(self):
        voice, provider = await self.start('realtime')
        self.auto_ack = False
        await provider.callback('input_end', {'item_id': 'input'})
        await eventually(lambda: voice.input_ack is not None)
        await self.adapter.command(self.peer, {'t': 'live_input_ack', 'turn': 6, 'on': False})
        self.assertFalse(voice.input_stopped.is_set())
        await self.adapter.command(self.peer, {'t': 'live_input_ack', 'turn': 7, 'on': True})
        self.assertFalse(voice.input_stopped.is_set())
        await self.adapter.command(self.peer, {'t': 'live_input_ack', 'turn': 7, 'on': False})
        await eventually(voice.input_stopped.is_set)

    async def test_playback_waits_for_full_generation_receipt(self):
        voice, provider = await self.start('realtime')
        voice.pause()
        await eventually(voice.input_stopped.is_set)
        self.auto_ack = False
        await provider.callback('audio', {'pcm': PCM, 'epoch': 0})
        await provider.callback('response_done', {'final': True})
        await eventually(lambda: any(isinstance(item, dict) and item['t'] == 'speak_end'
                                    for item in self.socket.messages))
        gen = self.peer.gen
        await self.adapter.command(self.peer, {'t': 'played', 'gen': gen, 'ms': 20})
        await asyncio.sleep(.01)
        self.assertIs(self.peer.native, voice)
        await self.adapter.command(self.peer, {'t': 'played', 'gen': gen + 1, 'ms': 40})
        self.assertIs(self.peer.native, voice)
        await self.adapter.command(self.peer, {'t': 'played', 'gen': gen, 'ms': 40})
        await eventually(lambda: self.peer.native is None)

    async def test_partial_pcm_block_uses_device_integer_millisecond_receipt(self):
        voice, provider = await self.start('realtime')
        voice.pause()
        await eventually(voice.input_stopped.is_set)
        await provider.callback('audio', {'pcm': PCM + bytes(22), 'epoch': 0})
        await provider.callback('response_done', {'final': True})
        await eventually(lambda: self.peer.native is None)
        self.assertEqual(self.peer.sent_ms, 40)

    async def test_live_cancel_ack_fences_next_epoch_audio(self):
        voice, provider = await self.start('live')
        await provider.callback('audio', {'pcm': PCM, 'epoch': 0})
        await eventually(lambda: self.peer.sent_ms == 40)
        old_gen = self.peer.gen
        self.auto_ack = False
        await provider.callback('input_activity', {'text': 'interrupt'})
        self.assertEqual(provider.epoch, 1, 'the next inbound delegation must see the new epoch')
        await eventually(lambda: any(isinstance(item, dict) and item['t'] == 'speak_cancel'
                                    for item in self.socket.messages))
        await provider.callback('audio', {'pcm': PCM, 'epoch': 0})
        await provider.callback('audio', {'pcm': PCM, 'epoch': 1})
        await asyncio.sleep(.01)
        self.assertEqual(self.peer.gen, old_gen)
        await self.adapter.command(self.peer, {'t': 'cancelled', 'gen': old_gen + 1})
        await asyncio.sleep(.01)
        self.assertEqual(self.peer.gen, old_gen)
        await self.adapter.command(self.peer, {'t': 'cancelled', 'gen': old_gen})
        await eventually(lambda: self.peer.gen != old_gen)
        self.assertEqual(self.peer.sent_ms, 40)
        self.auto_ack = True

    async def test_live_barge_preempts_final_receipt_wait(self):
        voice, provider = await self.start('live')
        await provider.callback('audio', {'pcm': PCM, 'epoch': 0})
        await eventually(lambda: self.peer.sent_ms == 40)
        self.auto_ack = False
        voice.output.begin_finish(False)
        await eventually(lambda: any(isinstance(item, dict) and item['t'] == 'speak_end'
                                    for item in self.socket.messages))
        self.assertTrue(voice.output.stream.ended)
        self.assertTrue(voice.output.speaking())
        await provider.callback('input_activity', {'text': 'interrupt while draining'})
        self.assertEqual(provider.epoch, 1)
        await eventually(lambda: any(isinstance(item, dict) and item['t'] == 'speak_cancel'
                                    for item in self.socket.messages))
        await self.adapter.command(self.peer, {'t': 'cancelled', 'gen': self.peer.gen})
        await eventually(lambda: voice.output.stream is None)
        self.assertFalse(voice.output.task.done())
        self.auto_ack = True
        await provider.callback('audio', {'pcm': PCM, 'epoch': 1})
        await eventually(lambda: voice.output.stream is not None and voice.output.stream.gen is not None)
        self.assertFalse(provider.closed)

    async def test_clear_during_speaking_state_write_drops_inflight_old_epoch_pcm(self):
        voice, provider = await self.start('live')
        entered, release = asyncio.Event(), asyncio.Event()
        original = self.socket.send_json
        async def delayed(value):
            if value.get('s') == 'speaking' and not entered.is_set():
                entered.set()
                await release.wait()
            await original(value)
        self.socket.send_json = delayed
        await provider.callback('audio', {'pcm': PCM, 'epoch': 0})
        await entered.wait()
        await provider.callback('input_activity', {'text': 'interrupt before first output frame'})
        release.set()
        await eventually(lambda: voice.output.clearing and voice.output.clearing.done())
        self.assertEqual([item for item in self.socket.messages if isinstance(item, bytes)], [])
        states = [item['s'] for item in self.socket.messages if isinstance(item, dict) and item['t'] == 'state']
        self.assertEqual(states[-1], 'listening')
        await provider.callback('audio', {'pcm': PCM, 'epoch': 1})
        await eventually(lambda: any(isinstance(item, bytes) for item in self.socket.messages))
        self.assertEqual(len([item for item in self.socket.messages if isinstance(item, bytes)]), 1)

    async def test_missing_cancel_ack_closes_transport_before_another_conversation(self):
        voice, provider = await self.start('live')
        await provider.callback('audio', {'pcm': PCM, 'epoch': 0})
        await eventually(lambda: self.peer.sent_ms == 40)
        self.auto_ack = False
        await voice.close()
        self.assertTrue(self.socket.closed)
        self.assertIsNone(self.peer.native)

    async def test_native_input_queue_allows_one_pending_job_and_drops_it_on_close(self):
        entered, release = asyncio.Event(), asyncio.Event()
        texts = []
        async def handle(event):
            texts.append(event.text)
            event._gateway_accepted = True
            async def background():
                entered.set()
                await release.wait()
                await self.adapter.on_processing_complete(event, 'success')
            asyncio.create_task(background())
        self.adapter.handle_message = handle
        voice, provider = await self.start('live')
        await provider.callback('delegation', {'id': 'first', 'text': 'first request'})
        await entered.wait()
        await provider.callback('delegation', {'id': 'second', 'text': 'second request'})
        with self.assertRaises(ValueError):
            await provider.callback('delegation', {'id': 'third', 'text': 'third request'})
        await voice.close()
        release.set()
        await eventually(lambda: not self.adapter.native_jobs.busy(self.identity))
        self.assertEqual(texts, ['first request'])

    async def test_live_quiet_pcm_never_opens_speaker_and_input_is_bounded(self):
        voice, provider = await self.start('live')
        await provider.callback('audio', {'pcm': bytes(1920), 'epoch': 0})
        await asyncio.sleep(.01)
        self.assertIsNone(voice.output.stream)
        for _ in range(125):
            voice.input_queue.put_nowait((PCM, bytes(1920)))
        with self.assertRaises(asyncio.QueueFull):
            voice.microphone(bytes((5, 7)) + encode(PCM, [0, 0]) + encode(bytes(1920), [0, 0]))

    async def test_text_mode_keeps_native_agent_output_visible_without_speech(self):
        async def handle(event):
            event._gateway_accepted = True
            await self.adapter.send(self.identity, 'Visible answer')
            await self.adapter.on_processing_complete(event, 'success')
        self.adapter.handle_message = handle
        self.peer.volume = 19
        voice, provider = await self.start('realtime')
        await provider.callback('transcript', {'text': 'hello', 'role': 'user', 'final': True})
        await eventually(lambda: self.peer.native is None)
        kinds = [value['t'] for value in self.socket.messages if isinstance(value, dict)]
        self.assertIn('text', kinds)
        self.assertNotIn('speak', kinds)

    async def test_live_text_mode_preserves_utf8_across_device_byte_limit(self):
        self.peer.volume = 19
        voice, provider = await self.start('live')
        text = 'я' * 750
        await provider.callback('transcript', {'text': text, 'role': 'assistant', 'final': False})
        await eventually(lambda: voice.text_task.done())
        shown = [value['text'] for value in self.socket.messages
                 if isinstance(value, dict) and value['t'] == 'text']
        self.assertEqual(''.join(shown), text)
        self.assertTrue(all(len(value.encode()) <= 1000 for value in shown))
