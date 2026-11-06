"""Real local aiohttp WebSockets; no credential or paid provider access."""
import asyncio
import base64
from dataclasses import FrozenInstanceError
from pathlib import Path
import struct
import sys
from types import ModuleType
import unittest

from aiohttp import web

ROOT = Path(__file__).resolve().parents[2]
package = ModuleType('hermes_kubik')
package.__path__ = [str(ROOT / 'hermes-kubik')]
sys.modules.setdefault(package.__name__, package)
from hermes_kubik.voice_provider import (MAX_AUDIO, ProviderError, ProviderLimits,
                                         ProviderSettings, create_provider, endpoint_url)
from hermes_kubik.live_provider import append_chunks

PCM = struct.pack('<h', 2000) * 2400
SILENCE = bytes(7200)


class ProviderFixture(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.received, self.events, self.providers = [], [], []
        self.changed = asyncio.Event()
        self.accept_ready = self.finish_response = self.accept_context = True
        self.sequence = 0
        self.socket = None
        app = web.Application()
        app.router.add_get('/voice', self.serve)
        self.runner = web.AppRunner(app)
        await self.runner.setup()
        self.site = web.TCPSite(self.runner, '127.0.0.1', 0)
        await self.site.start()
        port = self.site._server.sockets[0].getsockname()[1]
        self.endpoint = f'http://127.0.0.1:{port}/voice'

    async def asyncTearDown(self):
        for provider in self.providers:
            await provider.close()
            self.assertTrue(provider.http is None or provider.http.closed)
            self.assertTrue(provider.ws is None or provider.ws.closed)
        await self.runner.cleanup()

    async def serve(self, request):
        self.assertEqual(request.headers.get('Authorization'), 'Bearer fixture-secret')
        self.assertNotIn('OpenAI-Beta', request.headers)
        self.assertFalse(request.query)
        socket = web.WebSocketResponse()
        await socket.prepare(request)
        self.socket = socket
        async for frame in socket:
            event = frame.json()
            self.received.append(event)
            self.changed.set()
            await self.respond(socket, event)
        return socket

    async def respond(self, socket, event):
        kind = event['type']
        if self.accept_ready and kind in ('session.start', 'session.update'):
            ready = 'session.started' if kind == 'session.start' else 'session.updated'
            await socket.send_json({'type': ready})
        elif kind == 'response.create' and self.finish_response:
            await self.respond_audio(socket, event)
        elif self.accept_context and kind in ('session.commentary.append', 'session.thinking.append'):
            await socket.send_json({'type': kind + 'ed', 'client_event_id': event['event_id']})
        elif kind == 'session.close':
            await socket.send_json({'type': 'session.closed', 'reason': 'close_requested'})
            await socket.close()

    async def respond_audio(self, socket, event):
        self.sequence += 1
        response = {'id': f'response-{self.sequence}', 'metadata': event['response']['metadata']}
        await socket.send_json({'type': 'response.created', 'response': response})
        await socket.send_json({'type': 'response.output_audio.delta', 'response_id': 'old',
                                'item_id': 'old-item', 'delta': base64.b64encode(PCM).decode()})
        await socket.send_json({'type': 'response.output_audio.delta', 'response_id': response['id'],
                                'item_id': 'item', 'delta': base64.b64encode(PCM).decode()})
        await socket.send_json({'type': 'response.done', 'response': {**response, 'status': 'completed'}})
        await socket.send_json({'type': 'response.output_audio.delta', 'response_id': response['id'],
                                'item_id': 'item', 'delta': base64.b64encode(PCM).decode()})

    async def callback(self, kind, data):
        self.events.append((kind, data))
        self.changed.set()

    async def wait_for(self, predicate):
        async with asyncio.timeout(2):
            while not predicate():
                self.changed.clear()
                if not predicate():
                    await self.changed.wait()

    async def provider(self, mode, **limits):
        settings = ProviderSettings('fixture-secret', model='gpt-live-1' if mode == 'live' else 'gpt-realtime-2.1')
        provider = create_provider(mode, settings, self.callback, endpoint=self.endpoint,
                                   limits=ProviderLimits(connect=.3, response=.5, close=.1, **limits))
        self.providers.append(provider)
        await provider.start()
        return provider

    async def input_end(self):
        await self.socket.send_json({'type': 'input_audio_buffer.speech_stopped', 'item_id': 'input'})
        await self.socket.send_json({'type': 'input_audio_buffer.committed', 'item_id': 'input'})
        await self.socket.send_json({'type': 'conversation.item.input_audio_transcription.completed',
                                     'item_id': 'wrong', 'transcript': 'stale'})
        await self.socket.send_json({'type': 'conversation.item.input_audio_transcription.completed',
                                     'item_id': 'input', 'transcript': 'Привет'})
        await self.wait_for(lambda: any(kind == 'transcript' for kind, _ in self.events))

    async def transcript(self, text, start=0, end=100):
        await self.socket.send_json({'type': 'session.input_transcript.delta', 'delta': text,
                                     'start_ms': start, 'end_ms': end})

    async def delegate(self, key='task-1', offset=100):
        await self.socket.send_json({'type': 'session.delegation.created', 'offset_ms': offset,
                                     'delegation': {'id': key, 'type': 'delegation', 'target': 'client'}})

    async def audio(self, pcm=PCM):
        await self.socket.send_json({'type': 'session.output_audio.delta',
                                     'delta': base64.b64encode(pcm).decode()})

    def event_data(self, kind):
        return [data for current, data in self.events if current == kind]

    async def test_realtime_vad_native_text_serial_speech_and_late_generations(self):
        provider = await self.provider('realtime')
        session = self.received[0]['session']
        self.assertEqual(session['type'], 'realtime')
        self.assertEqual(session['model'], 'gpt-realtime-2.1')
        self.assertFalse(session['audio']['input']['turn_detection']['create_response'])
        self.assertEqual(session['audio']['input']['format']['rate'], 24000)
        self.assertEqual(session['audio']['output']['voice'], 'marin')
        await provider.append(PCM)
        await self.input_end()
        await provider.append(PCM)  # Device frames arriving after VAD never leak to output.
        await provider.commit()  # No duplicate commit after server VAD.
        await provider.reply('Первая часть.')
        await provider.reply('Вторая часть.', final=True)
        self.assertEqual([entry['text'] for entry in self.event_data('transcript')], ['Привет'])
        self.assertEqual(len(self.event_data('input_end')), 1)
        self.assertEqual(len(self.event_data('audio')), 2)
        self.assertTrue(self.event_data('response_done')[-1]['final'])
        kinds = [entry['type'] for entry in self.received]
        self.assertEqual(kinds.count('input_audio_buffer.append'), 1)
        self.assertNotIn('input_audio_buffer.commit', kinds)
        requests = [entry['response'] for entry in self.received if entry['type'] == 'response.create']
        self.assertEqual(len(requests), 2)
        self.assertTrue(all(item['conversation'] == 'none' and item['input'] == [] for item in requests))

    async def test_realtime_manual_commit_and_short_input_error(self):
        provider = await self.provider('realtime')
        await provider.append(PCM)
        await provider.commit()
        await provider.commit()
        await self.wait_for(lambda: any(item['type'] == 'input_audio_buffer.commit' for item in self.received))
        self.assertEqual(sum(item['type'] == 'input_audio_buffer.commit' for item in self.received), 1)
        await provider.close()
        other = await self.provider('realtime')
        await other.commit()
        self.assertEqual(other.failure, 'provider_input_too_short')
        self.assertTrue(other.stopping)

    async def test_live_current_wire_delegation_late_transcript_and_acknowledged_chunks(self):
        provider = await self.provider('live')
        config = self.received[0]
        self.assertEqual(config['type'], 'session.start')
        self.assertEqual(config['session']['delegation'], {'type': 'client'})
        self.assertEqual(config['session']['model'], 'gpt-live-1')
        await provider.append(PCM)
        await provider.commit()
        await self.delegate()
        await self.transcript('Привет,  мир')
        await self.wait_for(lambda: len(self.event_data('delegation')) == 1)
        self.assertEqual(self.event_data('delegation')[0]['text'], 'Привет,  мир')
        await self.delegate()  # Duplicate delegation must not execute Hermes twice.
        text = 'Проверенный результат. ' * 100
        await provider.reply(text, 'task-1', final=True)
        chunks = [entry for entry in self.received if entry['type'] == 'session.commentary.append']
        self.assertEqual(''.join(entry['content'] for entry in chunks), text)
        self.assertTrue(all(len(entry['content'].encode()) <= 400 for entry in chunks))
        self.assertTrue(all(entry['delegation_id'] == 'task-1' for entry in chunks))
        self.assertEqual(len(self.event_data('delegation')), 1)
        await self.audio(SILENCE)
        await self.wait_for(lambda: bool(self.event_data('audio')))
        self.assertEqual(self.event_data('audio')[0]['pcm'], SILENCE)
        self.assertFalse(self.event_data('response_done'))
        self.assertNotIn('response.create', [entry['type'] for entry in self.received])
        await provider.close()
        self.assertTrue(any(entry['type'] == 'session.close' for entry in self.received))

    async def test_live_interruption_clears_playback_preserves_work_and_continues_audio(self):
        provider = await self.provider('live')
        await self.transcript('Первая задача')
        await self.delegate()
        await self.wait_for(lambda: bool(self.event_data('delegation')))
        await self.audio()
        await self.wait_for(lambda: bool(self.event_data('audio')))
        await provider.interrupt(42)
        await self.audio()
        await provider.append(PCM)  # Microphone remains full duplex.
        await provider.reply('Первый результат', 'task-1', final=True)
        await self.wait_for(lambda: len(self.event_data('audio')) == 2)
        self.assertEqual([item['epoch'] for item in self.event_data('audio')], [0, 1])
        self.assertEqual(self.event_data('clear')[0]['consumed_ms'], 42)
        self.assertTrue(any(item['type'] == 'session.thinking.append' for item in self.received))
        await self.transcript('Новая задача', 200, 300)
        await self.delegate('task-2', 300)
        await self.wait_for(lambda: len(self.event_data('delegation')) == 2)
        await provider.reply('Новый результат', 'task-2')
        await self.audio()
        await self.wait_for(lambda: len(self.event_data('audio')) == 3)
        self.assertEqual(self.event_data('audio')[-1]['epoch'], 1)
        self.assertIsNone(provider.failure)

    async def test_live_direct_reply_without_quiet_delegation_or_ack_keeps_delivery_order(self):
        provider = await self.provider('live')
        await self.audio()
        await self.wait_for(lambda: bool(self.event_data('audio')))
        await provider.interrupt(0)
        first, second = struct.pack('<h', 3000) * 2400, struct.pack('<h', 4000) * 2400
        await self.transcript('Direct question', 200, 300)
        await self.audio(first)
        await self.audio(second)
        await provider.append(PCM)
        await self.wait_for(lambda: len(self.event_data('audio')) == 3)
        audio = self.event_data('audio')
        self.assertEqual([item['pcm'] for item in audio], [PCM, first, second])
        self.assertEqual([item['epoch'] for item in audio], [0, 1, 1])
        self.assertEqual(len(self.event_data('input_activity')), 1)
        self.assertFalse(self.event_data('delegation'))
        self.assertFalse(any(item['type'] in ('session.commentary.append', 'session.thinking.append')
                             for item in self.received))
        self.assertIsNone(provider.failure)

    async def test_live_interrupt_while_first_chunk_ack_is_pending_keeps_remaining_text_silent(self):
        provider = await self.provider('live')
        await self.transcript('Первая задача')
        await self.delegate()
        await self.wait_for(lambda: bool(self.event_data('delegation')))
        self.accept_context = False
        text = 'Проверенный результат. ' * 40
        pending = asyncio.create_task(provider.reply(text, 'task-1', final=True))
        await self.wait_for(lambda: any(item['type'] == 'session.commentary.append' for item in self.received))
        first = next(item for item in self.received if item['type'] == 'session.commentary.append')
        await provider.interrupt(42)
        self.accept_context = True
        await self.socket.send_json({'type': 'session.commentary.appended', 'client_event_id': first['event_id']})
        await pending
        chunks = [item for item in self.received if item['type'] in
                  ('session.commentary.append', 'session.thinking.append')]
        self.assertEqual(''.join(item['content'] for item in chunks), text)
        self.assertGreater(len(chunks), 1)
        self.assertEqual(chunks[0]['type'], 'session.commentary.append')
        self.assertTrue(all(item['type'] == 'session.thinking.append' for item in chunks[1:]))
        await self.audio()
        await self.socket.send_json({'type': 'session.output_transcript.delta', 'delta': 'old output',
                                     'start_ms': 100, 'end_ms': 200})
        await self.wait_for(lambda: any(item.get('role') == 'assistant' for item in self.event_data('transcript')))
        self.assertEqual([item['epoch'] for item in self.event_data('audio')], [1])
        await self.transcript('Новая задача', 200, 300)
        await self.delegate('task-2', 300)
        await self.wait_for(lambda: len(self.event_data('delegation')) == 2)
        await provider.reply('Новый ответ', 'task-2', final=True)
        await self.audio()
        await self.wait_for(lambda: len(self.event_data('audio')) == 2)
        self.assertEqual(self.event_data('audio')[-1]['epoch'], 1)

    async def test_live_rechecks_epoch_after_waiting_for_transport_writer(self):
        provider = await self.provider('live')
        await self.transcript('Question')
        await self.delegate()
        await self.wait_for(lambda: bool(self.event_data('delegation')))
        await provider._write_lock.acquire()
        try:
            pending = asyncio.create_task(provider.reply('Accepted result', 'task-1', final=True))
            interruption = asyncio.create_task(provider.interrupt(0))
            await self.wait_for(lambda: bool(self.event_data('clear')))
        finally:
            provider._write_lock.release()
        await asyncio.gather(pending, interruption)
        chunks = [item for item in self.received if item['type'] in
                  ('session.commentary.append', 'session.thinking.append')]
        self.assertEqual([item['type'] for item in chunks], ['session.thinking.append'])
        await self.audio()
        await self.wait_for(lambda: bool(self.event_data('audio')))
        self.assertEqual(self.event_data('audio')[0]['epoch'], 1)

    async def test_live_idle_after_interruption_preserves_input_and_slow_backend_work(self):
        provider = await self.provider('live')
        await self.transcript('First accepted task')
        await self.delegate()
        await self.wait_for(lambda: bool(self.event_data('delegation')))
        await provider.interrupt(0)
        # No silent deltas are required while the provider waits for a tool.
        await asyncio.sleep(3.05)
        self.assertIsNone(provider.failure)
        self.assertFalse(provider.stopping)
        self.assertFalse(self.event_data('audio'))
        await provider.append(PCM)
        await self.audio()
        await self.wait_for(lambda: bool(self.event_data('audio')))
        await provider.reply('First task completed once', 'task-1', final=True)
        self.assertTrue(any(item['type'] == 'session.thinking.append' for item in self.received))
        await self.transcript('New request', 200, 300)
        await self.delegate('task-2', 300)
        await self.wait_for(lambda: len(self.event_data('delegation')) == 2)
        await provider.reply('New acknowledged reply', 'task-2', final=True)
        await self.audio()
        await self.wait_for(lambda: len(self.event_data('audio')) == 2)
        self.assertEqual([item['epoch'] for item in self.event_data('audio')], [1, 1])
        self.assertFalse(self.event_data('error'))

    async def test_response_timeout_and_secret_safe_provider_error(self):
        self.finish_response = False
        provider = await self.provider('realtime')
        await provider.append(PCM)
        await self.input_end()
        await provider.reply('Answer')
        await self.wait_for(lambda: bool(self.event_data('error')))
        self.assertEqual(provider.failure, 'provider_response_timeout')
        self.assertTrue(provider.stopping)
        self.events.clear()
        other = await self.provider('live')
        await self.socket.send_json({'type': 'error', 'error': {'message': 'fixture-secret'}})
        await self.wait_for(lambda: bool(self.event_data('error')))
        self.assertEqual(other.failure, 'provider_rejected_request')
        self.assertNotIn('fixture-secret', repr(self.events))

    async def test_connection_timeout_unexpected_close_and_audio_validation(self):
        self.accept_ready = False
        with self.assertRaisesRegex(ProviderError, 'provider_connect_failed'):
            await self.provider('live')
        self.accept_ready = True
        self.events.clear()
        provider = await self.provider('live')
        with self.assertRaisesRegex(ProviderError, 'invalid_provider_audio'):
            await provider.append(b'odd')
        with self.assertRaisesRegex(ProviderError, 'invalid_provider_audio'):
            await provider.append(bytes(MAX_AUDIO + 2))
        await self.socket.close()
        await self.wait_for(lambda: bool(self.event_data('error')))
        self.assertEqual(provider.failure, 'provider_connection_closed')
        with self.assertRaises(ProviderError):
            await provider.append(PCM)

    async def test_live_context_timeout_and_missing_delegation_input_are_explicit(self):
        provider = await self.provider('live')
        await self.delegate()
        await self.wait_for(lambda: bool(self.event_data('error')))
        self.assertEqual(provider.failure, 'provider_delegation_input_missing')
        self.events.clear()
        self.accept_context = False
        provider = await self.provider('live')
        await self.transcript('Question')
        await self.delegate()
        await self.wait_for(lambda: bool(self.event_data('delegation')))
        with self.assertRaisesRegex(ProviderError, 'provider_context_timeout'):
            await provider.reply('Answer', 'task-1')

    async def test_session_expiry_and_late_events_do_not_cross_connections(self):
        old = await self.provider('live', session=.05)
        await self.wait_for(lambda: bool(self.event_data('error')))
        self.assertEqual(old.failure, 'provider_session_timeout')
        await old.close()
        self.events.clear()
        current = await self.provider('live')
        with self.assertRaises(ProviderError):
            await old.append(PCM)
        await current.append(PCM)
        await self.audio()
        await self.wait_for(lambda: bool(self.event_data('audio')))
        self.assertEqual(len(self.event_data('audio')), 1)
        self.assertIsNone(current.failure)

    async def test_unrequested_realtime_response_never_bypasses_hermes(self):
        provider = await self.provider('realtime')
        await self.socket.send_json({'type': 'response.created', 'response': {'id': 'unsolicited'}})
        await self.socket.send_json({'type': 'response.output_audio.delta', 'response_id': 'unsolicited',
                                     'item_id': 'item', 'delta': base64.b64encode(PCM).decode()})
        await provider.append(PCM)
        await self.input_end()
        self.assertFalse(self.event_data('audio'))

    async def test_invalid_wire_and_ack_id_are_rejected_without_raw_payload(self):
        provider = await self.provider('live')
        await self.socket.send_json({'type': 'session.output_audio.delta', 'delta': 'fixture-secret'})
        await self.wait_for(lambda: bool(self.event_data('error')))
        self.assertEqual(provider.failure, 'invalid_provider_audio')
        self.events.clear()
        provider = await self.provider('live')
        self.accept_context = False
        await self.transcript('Question')
        await self.delegate()
        await self.wait_for(lambda: bool(self.event_data('delegation')))
        pending = asyncio.create_task(provider.reply('Answer', 'task-1'))
        await self.wait_for(lambda: any(item['type'] == 'session.commentary.append' for item in self.received))
        await self.socket.send_json({'type': 'session.commentary.appended', 'client_event_id': 'old'})
        with self.assertRaisesRegex(ProviderError, 'provider_context_timeout'):
            await pending

    def test_endpoint_and_settings_are_immutable_and_bounded(self):
        settings = ProviderSettings('fixture-secret')
        self.assertNotIn('fixture-secret', repr(settings))
        with self.assertRaises(FrozenInstanceError):
            settings.model = 'other'
        self.assertEqual(endpoint_url(settings, 'realtime'),
                         'wss://api.openai.com/v1/realtime?model=gpt-realtime-2.1')
        self.assertEqual(endpoint_url(settings, 'live'), 'wss://api.openai.com/v1/live/sessions')
        for url in ('http://untrusted', 'https://user:pass@host/v1', 'https://host/v1?secret=x'):
            with self.assertRaises(ProviderError):
                endpoint_url(ProviderSettings('fixture-secret', base_url=url), 'live')
        self.assertEqual(''.join(append_chunks('🙂' * 1000)), '🙂' * 1000)


if __name__ == '__main__':
    unittest.main()
