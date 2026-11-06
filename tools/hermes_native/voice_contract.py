"""Native Gateway/AIAgent voice modes through the signed device and provider wires."""
import asyncio
import base64
import contextlib
import json

from aiohttp import WSMsgType
from cryptography.hazmat.primitives.asymmetric import ec
from voice_fixture import PCM, eventually


class Device:
    def __init__(self, socket, codec):
        self.socket, self.codec = socket, codec
        self.events = []
        self.audio = bytearray()
        self.played = {}
        self.sequence = 100
        self.reader = asyncio.create_task(self.read())

    async def read(self):
        async for frame in self.socket:
            if frame.type == WSMsgType.BINARY:
                assert frame.data[0] == 3
                pcm = self.codec.decode(frame.data[2:])
                self.audio.extend(pcm)
                gen = frame.data[1]
                self.played[gen] = self.played.get(gen, 0) + len(pcm) // 48
                self.events.append({'t': 'audio', 'gen': gen})
                await self.send(t='progress', gen=gen, ms=self.played[gen])
            elif frame.type == WSMsgType.TEXT:
                value = json.loads(frame.data)
                self.events.append(value)
                await self.acknowledge(value)

    async def acknowledge(self, value):
        kind = value['t']
        if kind == 'live_input':
            await self.send(t='live_input_ack', turn=value['turn'], on=value['on'])
        elif kind == 'text':
            await self.send(t='shown', receipt=value['receipt'])
        elif kind == 'speak_end':
            await self.send(t='played', gen=value['gen'], ms=self.played.get(value['gen'], 0))
        elif kind == 'speak_cancel':
            await self.send(t='cancelled', gen=value['gen'])

    async def send(self, **value):
        await self.socket.send_json(value)

    async def wait(self, predicate, start=0):
        def found():
            if self.reader.done():
                self.reader.result()
            return next((value for value in self.events[start:] if predicate(value)), None)
        await eventually(found)
        return found()

    async def select(self, target, choice=None):
        self.sequence += 1
        value = {'t': 'agent_options' if choice is None else 'agent_model',
                 'target': target, 'rid': self.sequence}
        if choice:
            value['id'] = choice
        start = len(self.events)
        await self.socket.send_json(value)
        result = await self.wait(lambda item: item.get('rid') == self.sequence, start)
        assert not result.get('error'), result
        if choice is not None:
            await self.wait(lambda item: item['t'] == 'capabilities', start)
        return result

    async def microphone(self, turn, live=False):
        mic = self.codec.encode(PCM[:1920], [0, 0])
        payload = bytes((5 if live else 4, turn)) + mic
        if live:
            payload += self.codec.encode(bytes(1920), [0, 0])
            assert len(payload) == 968
        await self.socket.send_bytes(payload)

    async def close(self):
        await self.socket.close()
        with contextlib.suppress(asyncio.CancelledError):
            await self.reader


def last_history(providers):
    return next(body['messages'] for kind, body in reversed(providers.calls) if kind == 'llm')


async def check_first_native(client, port, home, codec, providers, pair):
    """The physical fixture's first interaction must not acquire duplicate cold-start speech."""
    providers.native_delay = 3.2
    device = Device(await pair(client, port, home, ec.generate_private_key(ec.SECP256R1()),
                               name='kubik-first-native'), codec)
    try:
        await device.select('mode', 'realtime')
        index = len(providers.voice.connections)
        start = len(device.events)
        await device.send(t='ptt', on=True, turn=21)
        connection = await providers.voice.opened(index)
        await device.wait(lambda item: item.get('s') == 'listening', start)
        for _ in range(3):
            await device.microphone(21)
        await eventually(lambda: len(connection.events('input_audio_buffer.append')) == 3)
        await connection.finish_input('kubik-native-first-realtime')
        await device.wait(lambda item: item.get('s') == 'idle', start)
        requested = [item['response']['instructions'].split('\n', 1)[1]
                     for item in connection.events('response.create')]
        from gateway.run import t
        notice = t('gateway.notify.no_home_channel', platform='Kubik', sethome_cmd='/sethome')
        assert ''.join(requested) == notice + 'kubik-native-answer:first-realtime', requested
        assert ''.join(requested).count('kubik-native-answer:first-realtime') == 1
        assert providers.tool_calls.count('first-realtime') == 1
        print('PASS first native Realtime turn preserves the distinct Hermes onboarding notice and one agent answer', flush=True)
    finally:
        await device.close()


async def realtime(device, fixture, providers):
    start, index = len(device.events), len(fixture.connections)
    await device.send(t='ptt', on=True, turn=11)
    connection = await fixture.opened(index)
    await device.wait(lambda item: item.get('s') == 'listening', start)
    for _ in range(3):
        await device.microphone(11)
    await eventually(lambda: len(connection.events('input_audio_buffer.append')) == 3)
    await connection.finish_input('kubik-native-realtime')
    await device.wait(lambda item: item.get('s') == 'idle', start)
    assert device.audio
    events = device.events[start:]
    stopped = next(i for i, item in enumerate(events) if item['t'] == 'live_input' and item['on'] is False)
    spoken = next(i for i, item in enumerate(events) if item['t'] == 'audio')
    assert stopped < spoken, events
    assert any(item['t'] == 'input_end' for item in events)
    requests = connection.events('response.create')
    assert requests and 'kubik-native-answer:realtime' in json.dumps(requests), requests
    spoken = [item['response']['instructions'].split('\n', 1)[1] for item in requests]
    assert ''.join(spoken) == 'kubik-native-answer:realtime', spoken
    assert 'kubik-first' in json.dumps(last_history(providers))
    assert providers.tool_calls.count('realtime') == 1, providers.tool_calls
    assert not [item for item in events if item['t'] == 'error'], events


async def live_turn(device, connection, providers, marker, offset):
    before = len(connection.events('session.commentary.append'))
    await connection.delegate('kubik-native-' + marker, marker, offset)
    await eventually(lambda: len(connection.events('session.commentary.append')) > before)
    result = connection.events('session.commentary.append')[before:]
    assert 'kubik-native-answer:' + marker in json.dumps(result), result
    assert providers.tool_calls.count(marker) == 1, providers.tool_calls


async def live(device, fixture, providers):
    start, index = len(device.events), len(fixture.connections)
    await device.send(t='ptt', on=True, turn=12)
    connection = await fixture.opened(index)
    await device.wait(lambda item: item['t'] == 'live_input' and item['on'], start)
    await device.microphone(12, live=True)
    await eventually(lambda: bool(connection.events('session.input_audio.append')))
    encoded = connection.events('session.input_audio.append')[0]['audio']
    clean = base64.b64decode(encoded)
    assert len(clean) == 1920 and any(clean), 'Real AEC lost near-end input'
    await live_turn(device, connection, providers, 'live-first', 100)
    await connection.audio()
    await device.wait(lambda item: item['t'] == 'audio', start)
    await device.microphone(12, live=True)
    await eventually(lambda: len(connection.events('session.input_audio.append')) == 2)
    assert not [item for item in device.events[start:] if item['t'] == 'input_end']
    # An input transcript during speech clears playback but preserves the session and agent.
    await live_turn(device, connection, providers, 'live-second', 300)
    await device.wait(lambda item: item['t'] == 'speak_cancel', start)
    assert connection.events('session.instructions.append')
    await connection.audio(bytes(7200))
    second_start = len(device.events)
    await connection.audio()
    await device.wait(lambda item: item['t'] == 'audio', second_start)
    assert 'kubik-native-live-first' in json.dumps(last_history(providers))
    await device.microphone(12, live=True)
    await eventually(lambda: len(connection.events('session.input_audio.append')) == 3)
    await device.send(t='cancel', turn=12)
    await device.wait(lambda item: item.get('s') == 'idle', second_start)
    assert connection.events('session.close')
    assert not [item for item in device.events[start:] if item['t'] == 'error'], device.events[start:]


async def reject_missing_reference(device, fixture):
    start, index = len(device.events), len(fixture.connections)
    await device.send(t='ptt', on=True, turn=13)
    connection = await fixture.opened(index)
    await device.wait(lambda item: item['t'] == 'live_input' and item['on'], start)
    await device.microphone(13, live=False)
    await eventually(lambda: device.socket.closed)
    assert device.socket.close_code == 4002, device.socket.close_code
    assert not connection.events('session.input_audio.append'), 'Unreferenced audio reached Live'


async def accepted_work_survives_stop(device, fixture, providers, adapter):
    start, index = len(device.events), len(fixture.connections)
    await device.send(t='ptt', on=True, turn=14)
    connection = await fixture.opened(index)
    await device.wait(lambda item: item['t'] == 'live_input' and item['on'], start)
    await connection.delegate('kubik-native-cancel', 'cancel', 100)
    await eventually(lambda: providers.tool_calls.count('cancel') == 1)
    await device.send(t='cancel', turn=14)
    await device.wait(lambda item: item.get('s') == 'idle', start)
    assert not providers.cancel_release.is_set(), 'Tool finished before the conversation stopped'
    providers.cancel_release.set()
    await eventually(lambda: not adapter.native_jobs.jobs)
    assert 'native-tool:cancel' in json.dumps(last_history(providers))
    assert not connection.events('session.commentary.append')
    assert not [item for item in device.events[start:] if item['t'] in ('text', 'audio', 'error')]


async def check_native_voice(client, port, home, key, adapter, codec, providers, pair):
    original = len(providers.calls)
    providers.native_delay = 3.2  # Exercise progress delivery on a slow genuine Gateway tool turn.
    device = Device(await pair(client, port, home, key), codec)
    try:
        modes = await device.select('mode')
        assert {row['id'] for row in modes['models'] if row['available']} == {'classic', 'realtime', 'live'}, modes
        await device.select('mode', 'realtime')
        models = await device.select('voice')
        assert models['model'] == 'openai/gpt-realtime-2.1', models
        await device.select('voice', 'openai/gpt-realtime-2.1')
        await device.close()
        device = Device(await pair(client, port, home, key, mode='realtime'), codec)
        await realtime(device, providers.voice, providers)
        await device.select('mode', 'live')
        await device.select('voice', 'openai/gpt-live-1')
        await live(device, providers.voice, providers)
        await device.close()
        device = Device(await pair(client, port, home, key, mode='live'), codec)
        await accepted_work_survives_stop(device, providers.voice, providers, adapter)
        state = json.loads((home / 'kubik/voice.json').read_text())
        assert any(item == {'mode': 'live', 'realtime': 'openai/gpt-realtime-2.1', 'live': 'openai/gpt-live-1'}
                   for item in state['entries'].values()), state
        await reject_missing_reference(device, providers.voice)
        assert not [kind for kind, _ in providers.calls[original:] if kind in ('stt', 'tts')]
        print('PASS: native Realtime/Live WSS, persistent paired choices, real AIAgent tool/context, '
              'no Classic STT/TTS, AEC input, continuous microphone, barge-in and cancellation', flush=True)
    finally:
        await device.close()
