"""Official voice wire events over real, locally trusted TLS WebSockets."""
import asyncio
import base64
import json
import math
import struct

from aiohttp import web

PCM = b''.join(struct.pack('<h', round(5000 * math.sin(i / 11))) for i in range(2400))


async def eventually(predicate, timeout=30):
    async with asyncio.timeout(timeout):
        while not predicate():
            await asyncio.sleep(.01)


class VoiceFixture:
    def __init__(self, connection_factory=None):
        self.connections = []
        self.connection_factory = connection_factory or VoiceConnection

    async def serve(self, request):
        assert request.headers.get('Authorization') == 'Bearer kubik-voice-test-only'
        mode = 'live' if request.path.endswith('/live/sessions') else 'realtime'
        assert dict(request.query) == ({} if mode == 'live' else {'model': 'gpt-realtime-2.1'})
        socket = web.WebSocketResponse()
        await socket.prepare(request)
        connection = self.connection_factory(mode, socket)
        self.connections.append(connection)
        async for frame in socket:
            event = frame.json()
            connection.received.append(event)
            await connection.respond(event)
        return socket

    async def opened(self, index):
        await eventually(lambda: len(self.connections) > index)
        connection = self.connections[index]
        await eventually(lambda: bool(connection.received))
        return connection


class VoiceConnection:
    def __init__(self, mode, socket):
        self.mode, self.socket = mode, socket
        self.received = []
        self.sequence = 0

    def events(self, kind):
        return [event for event in self.received if event['type'] == kind]

    async def respond(self, event):
        kind = event['type']
        if kind in ('session.start', 'session.update'):
            ready = 'session.started' if self.mode == 'live' else 'session.updated'
            await self.socket.send_json({'type': ready})
        elif kind == 'response.create':
            await self.realtime_audio(event)
        elif kind in ('session.commentary.append', 'session.thinking.append'):
            await self.socket.send_json({'type': kind + 'ed', 'client_event_id': event['event_id']})
        elif kind == 'session.close':
            await self.socket.send_json({'type': 'session.closed', 'reason': 'close_requested'})
            await self.socket.close()

    async def realtime_audio(self, event):
        self.sequence += 1
        response = {'id': f'voice-{self.sequence}', 'metadata': event['response']['metadata']}
        await self.socket.send_json({'type': 'response.created', 'response': response})
        await self.socket.send_json({'type': 'response.output_audio.delta', 'response_id': response['id'],
                                    'item_id': 'spoken', 'delta': base64.b64encode(PCM).decode()})
        await self.socket.send_json({'type': 'response.done', 'response': {**response, 'status': 'completed'}})

    async def finish_input(self, text):
        for kind in ('input_audio_buffer.speech_stopped', 'input_audio_buffer.committed'):
            await self.socket.send_json({'type': kind, 'item_id': 'native-input'})
        await self.socket.send_json({'type': 'conversation.item.input_audio_transcription.completed',
                                    'item_id': 'native-input', 'transcript': text})

    async def delegate(self, text, identifier, offset):
        await self.socket.send_json({'type': 'session.input_transcript.delta', 'delta': text,
                                    'start_ms': offset - 100, 'end_ms': offset})
        event = {'type': 'session.delegation.created', 'offset_ms': offset,
                 'delegation': {'id': identifier, 'type': 'delegation', 'target': 'client'}}
        await self.socket.send_json(event)
        await self.socket.send_json(event)  # A retransmitted delegation must never repeat a tool.

    async def audio(self, pcm=PCM):
        await self.socket.send_json({'type': 'session.output_audio.delta',
                                    'delta': base64.b64encode(pcm).decode()})


def register_contract_tool(providers):
    """A harmless actual Hermes tool; the native AIAgent must invoke its registered handler."""
    from tools.registry import registry
    def handle(arguments, **_):
        providers.tool_calls.append(arguments['marker'])
        if arguments['marker'] == 'cancel':
            assert providers.cancel_release.wait(8), 'Native cancellation test did not release its tool'
        return json.dumps({'result': 'native-tool:' + arguments['marker']})
    registry.register(name='kubik_contract_probe', toolset='kubik_contract',
                      schema={'name': 'kubik_contract_probe', 'description': 'Return the supplied test marker.',
                              'parameters': {'type': 'object', 'properties': {'marker': {'type': 'string'}},
                                             'required': ['marker']}}, handler=handle)


def native_answer(messages):
    """An agent tool roundtrip is requested only for the current native user turn."""
    index = next(i for i in range(len(messages) - 1, -1, -1) if messages[i]['role'] == 'user')
    user = str(messages[index]['content'])
    if 'kubik-native-' not in user:
        return None
    marker = user.split('kubik-native-', 1)[1].split()[0]
    tools = [message for message in messages[index + 1:] if message['role'] == 'tool']
    if tools:
        assert 'native-tool:' + marker in json.dumps(tools), tools
        return {'role': 'assistant', 'content': 'kubik-native-answer:' + marker}, 'stop'
    call = {'id': 'call-' + marker, 'type': 'function',
            'function': {'name': 'kubik_contract_probe', 'arguments': json.dumps({'marker': marker})}}
    return {'role': 'assistant', 'content': None, 'tool_calls': [call]}, 'tool_calls'
