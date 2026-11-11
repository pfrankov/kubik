"""Real screen emoji cards and Live input through a genuine local Hermes agent; zero prints."""
import asyncio
import base64
import time
import uuid

from device_control import fault_window, save
from device_printer import MicConnection
from device_voice import begin
from voice_fixture import VoiceFixture, eventually

MARKER = 'kubik-screen-emoji-check-'
ONLY_MARKER = 'kubik-screen-emoji-only-'
SEQUENCES = ['😀', '❤️', '👍🏽', '👩‍💻', '🇷🇺', '😎', '🍎', '🚀', '🐱', '✅']
TEXT = 'Apple emoji on Kubik\n' + ' '.join(SEQUENCES[:5]) + '\n' + ' '.join(SEQUENCES[5:])
ONLY_TEXT = ''.join(SEQUENCES[:5])


def screen_answer(messages):
    user = next((row['content'] for row in reversed(messages) if row['role'] == 'user'), '')
    if ONLY_MARKER in str(user):
        return {'role': 'assistant', 'content': ONLY_TEXT}, 'stop'
    return ({'role': 'assistant', 'content': TEXT}, 'stop') if MARKER in str(user) else None


class ScreenConnection(MicConnection):
    def __init__(self, *args):
        super().__init__(*args)
        self.result_text, self.transcript_sent = '', False

    async def respond(self, event):
        await super().respond(event)
        if event['type'] != 'session.commentary.append':
            return
        self.result_text += event.get('content', '')
        if self.result_text == TEXT and not self.transcript_sent:
            self.transcript_sent = True
            await self.socket.send_json({'type': 'session.output_transcript.delta', 'delta': TEXT,
                                         'start_ms': 0, 'end_ms': 100})


class Observations:
    def __init__(self, adapter):
        self.adapter, self.wire, self.states, self.frames = adapter, [], [], []
        self.command, self.authenticate = adapter.command, adapter.authenticate
        self.senders, self.evidence, self.connection = [], None, None

    def install(self):
        async def authenticate(socket):
            peer = await self.authenticate(socket)
            self.watch_socket(socket)
            return peer
        async def command(peer, value):
            if value.get('t') == 'emoji_get':
                self.record('device', value)
            return await self.command(peer, value)
        self.adapter.authenticate, self.adapter.command = authenticate, command

    def record(self, direction, value):
        if len(self.wire) >= 128:
            raise AssertionError('Screen emoji wire observation limit')
        self.wire.append({'at': time.monotonic(), 'direction': direction, 'value': dict(value)})
        self.persist_wire()

    def watch_socket(self, socket):
        if any(existing is socket for existing, _ in self.senders):
            return
        original = socket.send_json
        self.senders.append((socket, original))
        async def send(value, **kwargs):
            if value.get('t') == 'printer':
                raise AssertionError('Screen fixture forbids every printer RPC before dispatch')
            if value.get('t') in ('text', 'emoji'):
                self.record('host', value)
            return await original(value, **kwargs)
        socket.send_json = send

    def persist_wire(self):
        if self.evidence:
            save(self.evidence / 'screen-emoji-wire.json', self.wire)
            save(self.evidence / 'screen-emoji-states.json', self.states)
            save(self.evidence / 'screen-emoji-frames.json', self.frames)
            save(self.evidence / 'screen-emoji-mic.json', self.connection.mic_times if self.connection else [])

    def persist(self, evidence, providers):
        self.evidence = evidence
        if self.connection is None and providers.voice.connections:
            self.connection = providers.voice.connections[-1]
        self.persist_wire()
        save(evidence / 'screen-emoji-model.json', [body for kind, body in providers.calls if kind == 'llm'])

    def restore(self):
        self.adapter.command, self.adapter.authenticate = self.command, self.authenticate
        for socket, original in self.senders:
            socket.send_json = original


async def pages(control, observations):
    ready = set()
    async with asyncio.timeout(40):
        while len(ready) < 3:
            state = await control.info()
            fields = ('emoji_id', 'emoji_count', 'emoji_ready', 'emoji_missing', 'card_page', 'card_pages',
                      'heap_internal', 'heap_failures', 'live_active', 'mic_open', 'mic_rx_enabled')
            assert all(field in state for field in fields), 'Screen emoji firmware diagnostics incomplete'
            row = {'at': time.monotonic(), **{field: state[field] for field in fields}}
            observations.states.append(row)
            observations.persist_wire()
            assert row['live_active'] and row['mic_open'] and row['mic_rx_enabled'], row
            assert not row['emoji_missing'], row
            if row['emoji_count'] and row['emoji_ready'] == row['emoji_count']:
                ready.add(row['card_page'])
                if not any(frame['page'] == row['card_page'] for frame in observations.frames):
                    await frame_snapshot(control, observations, row['card_page'])
            await asyncio.sleep(.25)
    return ready


async def frame_snapshot(control, observations, page):
    try:
        value = await control.sim('stats')
        observations.frames.append({'page': page, 'metrics': value})
    except Exception as error:
        observations.frames.append({'page': page, 'unavailable': type(error).__name__})
    observations.persist_wire()


def verify_replies(observations, replies):
    seen = set()
    for row in replies:
        value = row['value']
        assert value['data'] is not None and value['seq'] in SEQUENCES, value
        glyph = base64.b64decode(value['data'], validate=True)
        assert len(glyph) == 576 and glyph == observations.adapter.screen_emoji.atlas.entries[value['seq']]
        seen.add(value['seq'])
    assert seen == set(SEQUENCES), seen
    return len(seen)


def verify(observations, baseline):
    requests = [row for row in observations.wire if row['value']['t'] == 'emoji_get']
    replies = [row for row in observations.wire if row['value']['t'] == 'emoji']
    texts = [row['value']['text'] for row in observations.wire if row['value']['t'] == 'text']
    assert TEXT in texts and len(requests) >= 3, texts
    assert all(1 <= len(row['value']['seq']) <= 4 for row in requests), requests
    received = verify_replies(observations, replies)
    start, end = requests[0]['at'], replies[-1]['at']
    mic = [at for at in observations.connection.mic_times if start < at < end]
    assert mic, 'Provider input stopped during color glyph requests'
    assert all(row['heap_failures'] == baseline['heap_failures'] for row in observations.states)
    return {'glyphs_received': received, 'page_requests': len(requests), 'mic_frames_during_requests': len(mic)}


async def check(adapter, peer, control, providers, observations):
    observations.evidence = control.evidence
    assert peer.screen_emoji_version == 1, 'Physical Cube requires screen_emoji:1'
    before = await control.request(cmd='bridge-diagnostics')
    baseline = await control.info()
    providers.voice = VoiceFixture(ScreenConnection)
    old_volume = peer.volume
    peer.volume = 0
    try:
        connection = observations.connection = await begin('live', adapter, peer, control, providers.voice)
        await control.sim('ptt_up')
        token = uuid.uuid4().hex[:16]
        await connection.delegate(MARKER + token, 'screen-emoji-' + token, 100)
        await eventually(lambda: connection.transcript_sent)
        ready = await pages(control, observations)
        measured = verify(observations, baseline)
        fault_window(before, await control.request(cmd='bridge-diagnostics'))
        return {'screen_emoji': True, 'native_agent_reply': True, 'ready_pages': len(ready),
                'live_continuous': True, 'test_prints_submitted': 0, 'paid_provider_calls': 0, **measured}
    finally:
        peer.volume = old_volume
        observations.persist(control.evidence, providers)
