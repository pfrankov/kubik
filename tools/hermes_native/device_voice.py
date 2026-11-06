"""Physical native voice, real device receipts and canonical AEC observation."""
import asyncio
import base64
import importlib
import json
import math
import struct
import time

from device_control import fault_window, playback_window, save
from voice_fixture import VoiceConnection, VoiceFixture, eventually


def profile_pcm(duration=8000):
    samples = []
    for index in range(duration * 24):
        moment = index / 24000
        envelope = .08 + .82 * ((1 + math.sin(moment * 2 * math.pi * 3.7)) / 2) ** 2
        sample = envelope * 15000 * (math.sin(moment * 2 * math.pi * 180) +
                                     .25 * math.sin(moment * 2 * math.pi * 360))
        samples.append(round(sample))
    return struct.pack('<' + 'h' * len(samples), *samples)


class PhysicalConnection(VoiceConnection):
    def __init__(self, *args):
        super().__init__(*args)
        self.expected_text = 'kubik-native-answer:physical-realtime'
        self.spoken_text = ''
        self.generated_pcm_bytes = 0
        self.profile = profile_pcm()

    def speech_slice(self, text):
        """One exact agent answer owns eight seconds, independently of Hermes's block boundaries."""
        previous = len(self.spoken_text)
        if not text or not self.expected_text.startswith(self.spoken_text + text):
            raise AssertionError('Unexpected or duplicated physical speech text: ' + repr(text))
        self.spoken_text += text
        # Keep PCM blocks aligned to two samples; concatenation still covers the exact full profile.
        pairs = len(self.profile) // 4
        start = previous * pairs // len(self.expected_text) * 4
        end = len(self.spoken_text) * pairs // len(self.expected_text) * 4
        pcm = self.profile[start:end]
        self.generated_pcm_bytes += len(pcm)
        return pcm

    async def paced(self, *, realtime_id=None, pcm=None):
        pcm, started = self.profile if pcm is None else pcm, time.monotonic()
        for offset in range(0, len(pcm), 4800):
            event = {'type': 'session.output_audio.delta',
                     'delta': base64.b64encode(pcm[offset:offset + 4800]).decode()}
            if realtime_id:
                event.update(type='response.output_audio.delta', response_id=realtime_id, item_id='physical-reply')
            await self.socket.send_json(event)
            await asyncio.sleep(max(0, started + min(offset + 4800, len(pcm)) / 48000 - time.monotonic()))
        if not realtime_id:
            await self.audio(bytes(144000))  # Real Live silence closes the local utterance, not Live.

    async def realtime_audio(self, event):
        self.sequence += 1
        text = event['response']['instructions'].split('\n', 1)[1]
        pcm = self.speech_slice(text)
        response = {'id': f'physical-{self.sequence}', 'metadata': event['response']['metadata']}
        await self.socket.send_json({'type': 'response.created', 'response': response})
        await self.paced(realtime_id=response['id'], pcm=pcm)
        await self.socket.send_json({'type': 'response.done', 'response': {**response, 'status': 'completed'}})


class Observations:
    def __init__(self, adapter):
        self.adapter, self.receipts, self.frames = adapter, [], []
        self.deliveries = []
        self.send_timing, self.socket_senders = [], []
        self.command = adapter.command
        self.send = adapter.send
        self.authenticate = adapter.authenticate
        package = adapter.__class__.__module__.rsplit('.', 1)[0]
        self.echo_class = importlib.import_module(package + '.echo').EchoCleaner
        self.process = self.echo_class.process

    def install(self):
        async def authenticate(socket):
            peer = await self.authenticate(socket)
            self.watch_socket(socket)
            return peer
        async def send(*args, **kwargs):
            if len(self.deliveries) >= 128:
                raise ValueError('Physical delivery observation limit')
            content = kwargs.get('content', args[1] if len(args) > 1 else '')
            metadata = kwargs.get('metadata') or {}
            self.deliveries.append({'text': str(content), 'metadata': {
                key: value for key, value in metadata.items() if isinstance(value, (str, int, bool, type(None)))}})
            return await self.send(*args, **kwargs)
        async def command(peer, value):
            if value.get('t') in ('played', 'progress', 'cancelled', 'live_input_ack'):
                if len(self.receipts) >= 4096:
                    raise ValueError('Physical receipt observation limit')
                self.receipts.append(dict(value))
            return await self.command(peer, value)
        def process(cleaner, pcm, reference):
            cleaned = self.process(cleaner, pcm, reference)
            if len(self.frames) < 750:
                self.frames.append((bytes(pcm), bytes(reference), bytes(cleaned)))
            return cleaned
        self.adapter.command = command
        self.adapter.send = send
        self.adapter.authenticate = authenticate
        self.echo_class.process = process

    def watch_socket(self, socket):
        if any(existing is socket for existing, _ in self.socket_senders):
            return
        original = socket.send_bytes
        self.socket_senders.append((socket, original))
        async def timed_send(payload):
            if len(self.send_timing) >= 4096:
                raise ValueError('Physical send timing observation limit')
            item = {'start': time.monotonic(), 'bytes': len(payload),
                    'gen': payload[1] if len(payload) >= 2 else None}
            self.send_timing.append(item)
            try:
                return await original(payload)
            finally:
                item['end'] = time.monotonic()
        socket.send_bytes = timed_send

    def restore(self):
        self.adapter.command = self.command
        self.adapter.send = self.send
        self.adapter.authenticate = self.authenticate
        for socket, original in self.socket_senders:
            socket.send_bytes = original
        self.echo_class.process = self.process

    def persist(self, evidence, providers):
        save(evidence / 'receipts.json', self.receipts)
        save(evidence / 'deliveries.json', self.deliveries)
        save(evidence / 'send-timing.json', timing_summary(self.send_timing))
        sessions = [{'mode': connection.mode, 'spoken_text': connection.spoken_text,
                     'generated_pcm_bytes': connection.generated_pcm_bytes,
                     'events': [event for event in connection.received
                    if event['type'] in ('response.create', 'session.commentary.append', 'session.thinking.append')]}
                    for connection in providers.voice.connections]
        save(evidence / 'voice-requests.json', sessions)
        save(evidence / 'provider-calls.json', [{'kind': kind} for kind, _ in providers.calls])

    def events(self, kind):
        return [value for value in self.receipts if value['t'] == kind]

    def acoustic(self, evidence):
        frames = self.frames[75:-25]
        assert frames, 'Physical AEC capture is incomplete'
        power = [0, 0, 0]
        count = 0
        for frame in frames:
            for channel, pcm in enumerate(frame):
                power[channel] += sum(sample[0] ** 2 for sample in struct.iter_unpack('<h', pcm))
            count += len(frame[0]) // 2
        for channel, label in enumerate(('mic', 'reference', 'clean')):
            save(evidence / (label + '.pcm'), b''.join(frame[channel] for frame in self.frames))
        result = {'frames': len(frames), 'reference_rms': math.sqrt(power[1] / count),
                  'erle_db': 10 * math.log10(max(1, power[0]) / max(1, power[2]))}
        save(evidence / 'acoustic.json', result)
        assert result['reference_rms'] > 100, 'Physical speaker reference is absent'
        assert result['erle_db'] > 10, 'Physical echo suppression did not exceed 10 dB'
        print('MEASURE actual Hermes AEC:', json.dumps(result), flush=True)
        return result


def timing_summary(frames):
    generations = {}
    for frame in frames:
        rows = generations.setdefault(str(frame['gen']), [])
        rows.append(frame)
    summaries = {}
    for generation, rows in generations.items():
        gaps = [right['start'] - left['start'] for left, right in zip(rows, rows[1:])]
        summaries[generation] = {'count': len(rows), 'longest_start_gap_ms': max(gaps, default=0) * 1000,
                                'longest_send_ms': max(item['end'] - item['start'] for item in rows) * 1000}
    return {'count': len(frames), 'generations': summaries, 'frames': frames}


async def set_mode(adapter, peer, control, mode):
    assert peer.native is None and not adapter.native_jobs.busy(peer.identity)
    adapter.voice_controls.store.select(peer.identity, 'mode', mode)
    await adapter.voice_controls.publish(peer)
    await control.until(lambda state: state['voice_mode'] == mode and not state['mic_open'], mode + ' applied')


async def begin(mode, adapter, peer, control, fixture):
    await set_mode(adapter, peer, control, mode)
    index = len(fixture.connections)
    await control.sim('ptt_down')
    connection = await fixture.opened(index)
    await control.until(lambda state: state['mic_open'] and state['mic_rx_enabled'], mode + ' microphone')
    event = 'session.input_audio.append' if mode == 'live' else 'input_audio_buffer.append'
    await eventually(lambda: len(connection.events(event)) >= 5)
    return connection


async def completion(control, previous, label, duration=8000):
    async with asyncio.timeout(18):
        while True:
            current = await control.request(cmd='speech')
            if current['sequence'] > previous['sequence'] and current['active'] < 0:
                break
            await asyncio.sleep(.15)
    print('PLAYBACK ' + label + ': ' + json.dumps(current), flush=True)
    save(control.evidence / (label + '-playback.json'), current)
    playback_window(previous, current, duration)


async def realtime(adapter, peer, control, providers):
    previous = await control.request(cmd='speech')
    connection = await begin('realtime', adapter, peer, control, providers.voice)
    await connection.finish_input('kubik-native-physical-realtime')
    await control.until(lambda state: not state['mic_open'] and not state['mic_enabled'],
                        'Realtime capture closes before playback')
    await completion(control, previous, 'realtime')
    await control.sim('ptt_up')
    await eventually(lambda: peer.native is None and not adapter.native_jobs.busy(peer.identity))
    assert providers.tool_calls.count('physical-realtime') == 1
    assert connection.spoken_text == connection.expected_text, connection.spoken_text
    assert connection.generated_pcm_bytes == 8000 * 48
    print('PASS physical Hermes Realtime microphone, real agent/tool and speaker receipts', flush=True)


async def delegate(connection, providers, marker, offset):
    before = len(connection.events('session.commentary.append'))
    await connection.delegate('kubik-native-' + marker, marker, offset)
    await eventually(lambda: len(connection.events('session.commentary.append')) > before)
    assert 'kubik-native-answer:' + marker in json.dumps(connection.events('session.commentary.append')[before:])
    assert providers.tool_calls.count(marker) == 1


async def barge(control, connection, observations, providers):
    old_count = len(observations.events('cancelled'))
    previous = await control.request(cmd='speech')
    await connection.audio(profile_pcm(3000))
    await control.until(lambda state: state['mic_open'] and state['live_ready'], 'Live before barge-in')
    await eventually(lambda: observations.events('progress') and
                     observations.events('progress')[-1]['gen'] != previous['gen'] and
                     observations.events('progress')[-1]['ms'] > 0)
    # This is a provider transcript of incoming audio, not an injected firmware cancel event.
    await delegate(connection, providers, 'physical-live-second', 300)
    await eventually(lambda: len(observations.events('cancelled')) > old_count)
    await connection.audio(profile_pcm(500))
    await connection.audio(bytes(144000))
    # The cancelled first generation also advances diagnostics; require replacement's played result.
    async with asyncio.timeout(15):
        while True:
            current = await control.request(cmd='speech')
            if current['sequence'] > previous['sequence'] and current['reason'] == 'played' and current['active'] < 0:
                break
            await asyncio.sleep(.15)
    playback_window(previous, current, 500)
    save(control.evidence / 'live-replacement-playback.json', current)
    await control.until(lambda state: state['live_active'] and state['live_ready'] and state['mic_open'],
                        'Live remains ready after replacement')
    print('PASS physical Hermes Live barge-in cancels exact playback and plays 500 ms replacement', flush=True)


async def live(adapter, peer, control, providers, observations):
    previous = await control.request(cmd='speech')
    connection = await begin('live', adapter, peer, control, providers.voice)
    await control.sim('ptt_up')
    await delegate(connection, providers, 'physical-live-first', 100)
    observations.frames.clear()
    count = len(connection.events('session.input_audio.append'))
    output = asyncio.create_task(connection.paced())
    await asyncio.sleep(1)
    state = await control.info()
    assert state['live_active'] and state['mic_open'], 'Live closed input while speaker was active'
    await output
    await completion(control, previous, 'live')
    assert len(connection.events('session.input_audio.append')) > count + 150, 'Live PCM stopped during output'
    acoustic = observations.acoustic(control.evidence)
    await barge(control, connection, observations, providers)
    await control.sim('ptt_down')
    await control.sim('ptt_up')
    await control.until(lambda state: not state['live_active'] and not state['mic_open'], 'KEY ends Live')
    await eventually(lambda: peer.native is None and not adapter.native_jobs.busy(peer.identity))
    assert connection.events('session.close')
    return acoustic


async def check(adapter, peer, control, providers, observations):
    providers.voice = VoiceFixture(PhysicalConnection)
    before = await control.request(cmd='bridge-diagnostics')
    start = len(providers.calls)
    await realtime(adapter, peer, control, providers)
    acoustic = await live(adapter, peer, control, providers, observations)
    assert not [kind for kind, _ in providers.calls[start:] if kind in ('stt', 'tts')]
    assert len(providers.voice.connections) == 2, 'Live provider reopened between replies'
    fault_window(before, await control.request(cmd='bridge-diagnostics'))
    save(control.evidence / 'receipts.json', observations.receipts)
    return {'realtime': True, 'live': True, 'barge_in': True, 'native_agent_tools': providers.tool_calls,
            'provider_connections': 2, 'acoustic': acoustic, 'paid_provider_calls': 0}
