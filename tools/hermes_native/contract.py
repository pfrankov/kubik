"""Real Hermes Gateway and AIAgent; only external HTTP providers are deterministic fixtures."""
import asyncio
import base64
import hashlib
import importlib
import io
import ipaddress
import json
import logging
import math
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import wave

from aiohttp import web, ClientSession, WSMsgType
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parents[2]
BLOCKED = set()


def network_guard(event, args):
    if event == 'socket.connect' and isinstance(args[1], tuple):
        host = args[1][0]
        try:
            allowed = ipaddress.ip_address(host).is_loopback
        except ValueError:
            allowed = host == 'localhost'
        if not allowed:
            BLOCKED.add(str(host))
            raise PermissionError('Hermes integration tests allow loopback connections only')


def reserve_loopback_ports(count):
    reservations = []
    try:
        for _ in range(count):
            sock = socket.socket()
            reservations.append(sock)
            sock.bind(('127.0.0.1', 0))
        return reservations
    except BaseException:
        for sock in reservations:
            sock.close()
        raise


def release_reservation(sock):
    if sock.fileno() >= 0:
        sock.close()


def wav_bytes(samples=2401):
    output = io.BytesIO()
    with wave.open(output, 'wb') as stream:
        stream.setparams((1, 2, 24000, 0, 'NONE', 'not compressed'))
        stream.writeframes(b''.join(struct.pack('<h', round(7000 * math.sin(i / 7))) for i in range(samples)))
    return output.getvalue()


class Providers:
    def __init__(self):
        self.calls = []
        self.transcript = 'kubik-first'

    async def respond(self, request):
        if request.path.endswith('/models/kubik-test'):
            return web.json_response({'id': 'kubik-test', 'object': 'model', 'context_length': 131072})
        if request.path == '/api/show':
            return web.json_response({'model_info': {'test.context_length': 131072}})
        if request.path.endswith('/models'):
            return web.json_response({'object': 'list', 'data': [{'id': 'kubik-test', 'object': 'model', 'owned_by': 'test'}]})
        if request.path.endswith('/audio/transcriptions'):
            await request.read()
            self.calls.append(('stt', self.transcript))
            return web.Response(text=self.transcript)
        body = await request.json()
        if request.path.endswith('/audio/speech'):
            self.calls.append(('tts', body))
            return web.Response(body=wav_bytes(), content_type='audio/wav')
        if request.path.endswith('/chat/completions'):
            self.calls.append(('llm', body))
            messages = body['messages']
            user = next((item['content'] for item in reversed(messages) if item['role'] == 'user'), '')
            answer = 'kubik-answer:second' if 'kubik-second' in str(user) else 'kubik-answer:first'
            data = {'id': 'chatcmpl-kubik', 'created': int(time.time()), 'model': body.get('model', 'kubik-test')}
            if body.get('stream'):
                response = web.StreamResponse(headers={'Content-Type': 'text/event-stream'})
                await response.prepare(request)
                for delta, finish in (({'role': 'assistant', 'content': answer}, None), ({}, 'stop')):
                    chunk = {**data, 'object': 'chat.completion.chunk', 'choices': [{'index': 0, 'delta': delta, 'finish_reason': finish}]}
                    await response.write(('data: ' + json.dumps(chunk) + '\n\n').encode())
                await response.write(b'data: [DONE]\n\n')
                await response.write_eof()
                return response
            return web.json_response({**data, 'object': 'chat.completion', 'choices': [{'index': 0,
                'message': {'role': 'assistant', 'content': answer}, 'finish_reason': 'stop'}],
                'usage': {'prompt_tokens': 25, 'completion_tokens': 5, 'total_tokens': 30}})
        raise AssertionError(f'Unexpected provider endpoint: {request.path}')


async def get_json(ws):
    message = await ws.receive(timeout=30)
    if message.type != WSMsgType.TEXT:
        raise AssertionError(f'Unexpected WebSocket frame: {message}')
    return json.loads(message.data)


async def pair(client, port, home, private_key, name='kubik-integration', volume=100, *, proof='valid'):
    ws = await client.ws_connect(f'wss://127.0.0.1:{port}/kubik/v1', ssl=False, compress=0)
    key = base64.b64encode(private_key.public_key().public_bytes(serialization.Encoding.X962,
        serialization.PublicFormat.UncompressedPoint)).decode()
    await ws.send_json({'t': 'hello', 'v': 5, 'device': name, 'key': key, 'volume': volume})
    challenge = await get_json(ws)
    assert challenge['t'] == 'challenge', challenge
    cert = x509.load_pem_x509_certificate((home / 'kubik/cert.pem').read_bytes())
    pin = hashlib.sha256(cert.public_key().public_bytes(serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo)).hexdigest()
    assert proof in ('valid', 'wrong-binding', 'invalid-signature')
    binding = '0' * 64 if proof == 'wrong-binding' else pin
    signed = '\n'.join(('kubik-auth-v5', challenge['nonce'], name, key, binding)).encode()
    signature = private_key.sign(signed, ec.ECDSA(hashes.SHA256()))
    if proof == 'invalid-signature':
        signature = signature[:-1] + bytes((signature[-1] ^ 1,))
    await ws.send_json({'t': 'auth', 'sig': base64.b64encode(signature).decode()})
    if proof != 'valid':
        rejected = await ws.receive(timeout=30)
        # InvalidSignature uses the existing 4001 failure close, not the malformed-JSON 4002 path.
        assert rejected.type == WSMsgType.CLOSE and rejected.data == 4001, rejected
        await ws.close()
        return None
    reply = await get_json(ws)
    if reply['t'] == 'pair':
        process = await asyncio.create_subprocess_exec(sys.executable, str(home / 'plugins/kubik/pair.py'), reply['code'],
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        stdout, stderr = await process.communicate()
        assert process.returncode == 0, (stdout, stderr)
        reply = await get_json(ws)
    assert reply['t'] == 'welcome', reply
    capabilities = await get_json(ws)
    assert capabilities['voice_mode'] == 'classic', capabilities
    assert capabilities['stt']['available'] and capabilities['tts']['available'], capabilities
    return ws


async def voice(ws, codec, turn, expected):
    await ws.send_json({'t': 'ptt', 'on': True, 'turn': turn})
    pcm = wav_bytes(24000)[44:]
    state = [0, 0]
    for start in range(0, len(pcm), 1920):
        await ws.send_bytes(bytes((4, turn)) + codec.encode(pcm[start:start + 1920], state))
    await ws.send_json({'t': 'ptt', 'on': False, 'turn': turn, 'ms': 1000})
    texts, audio, played = [], bytearray(), 0
    async with asyncio.timeout(90):
        while True:
            message = await ws.receive()
            if message.type == WSMsgType.BINARY:
                assert message.data[0] == 3
                audio.extend(codec.decode(message.data[2:]))
                played = len(audio) // 48
                await ws.send_json({'t': 'progress', 'gen': message.data[1], 'ms': played})
                continue
            assert message.type == WSMsgType.TEXT, message
            value = json.loads(message.data)
            kind = value['t']
            assert kind != 'error', value
            if kind == 'text':
                texts.append(value['text'])
                await ws.send_json({'t': 'shown', 'receipt': value['receipt']})
            elif kind == 'speak_end':
                await ws.send_json({'t': 'played', 'gen': value['gen'], 'ms': played})
            elif kind == 'state' and value['s'] == 'idle' and texts:
                break
    assert any(expected in text for text in texts), texts
    return audio


def configure(home, backend_port):
    from ruamel.yaml import YAML
    url = f'http://127.0.0.1:{backend_port}/v1'
    data = {'model': {'provider': 'custom', 'default': 'kubik-test', 'base_url': url, 'api_key': 'kubik-test-only', 'context_length': 131072},
        'plugins': {'isolation': 'in_process'}, 'gateway': {'multiplex_profiles': False},
        'stt': {'enabled': True, 'provider': 'openai', 'openai': {'model': 'whisper-1', 'base_url': url, 'api_key': 'kubik-test-only'}},
        'tts': {'provider': 'openai', 'openai': {'model': 'tts-1', 'voice': 'alloy', 'base_url': url, 'api_key': 'kubik-test-only'}},
        'agent': {'max_turns': 3}, 'memory': {'enabled': False}, 'toolsets': {'kubik': []}}
    with (home / 'config.yaml').open('w') as stream:
        YAML().dump(data, stream)
    (home / '.env').write_text(f'OPENAI_API_KEY=kubik-test-only\nOPENAI_BASE_URL={url}\n')
    os.environ.update(OPENAI_API_KEY='kubik-test-only', OPENAI_BASE_URL=url, TELEGRAM_ALLOWED_USERS='123')



async def check_profile_scopes(adapter, home):
    from hermes_constants import get_hermes_home
    from agent.secret_scope import get_secret
    runtime = importlib.import_module(adapter.__class__.__module__.rsplit('.', 1)[0] + '.runtime')
    original_home = get_hermes_home()
    original_env = dict(os.environ)
    async def read_scope(name, port):
        profile = home / 'profiles' / name
        profile.mkdir(parents=True)
        (profile / 'config.yaml').write_text('model: kubik-test\n')
        (profile / '.env').write_text(f'KUBIK_PORT={port}\nOPENAI_API_KEY={name}-test-secret\n')
        with runtime.profile_scope(profile):
            await asyncio.sleep(0)
            assert get_hermes_home() == profile
            assert get_secret('OPENAI_API_KEY') == name + '-test-secret'
            assert runtime.listener_options(adapter.config) == ('127.0.0.1', port)
            assert await asyncio.to_thread(get_hermes_home) == profile
        assert get_hermes_home() == original_home
    await asyncio.gather(read_scope('first', 28791), read_scope('second', 28792))
    assert dict(os.environ) == original_env


async def main():
    import tornado
    assert (6, 5, 9) <= tornado.version_info[:3] < (7, 0, 0), 'Install hermes-kubik/requirements.txt before native acceptance'
    sys.addaudithook(network_guard)
    home = Path(os.environ['HERMES_HOME'])
    home.mkdir(mode=0o700, parents=True, exist_ok=False)
    providers = Providers()
    backend_reservation, device_reservation = reserve_loopback_ports(2)
    backend_port = backend_reservation.getsockname()[1]
    device_port = device_reservation.getsockname()[1]
    configure(home, backend_port)
    subprocess.run([sys.executable, str(ROOT / 'tools/install-hermes.py'), '--home', str(home),
                    '--bind', '127.0.0.1', '--port', str(device_port)], check=True)
    app = web.Application(client_max_size=5 * 1024 * 1024)
    app.router.add_route('*', '/{tail:.*}', providers.respond)
    backend = web.AppRunner(app)
    await backend.setup()
    release_reservation(backend_reservation)
    await web.TCPSite(backend, '127.0.0.1', backend_port).start()
    from hermes_cli.plugins import get_plugin_manager
    get_plugin_manager().discover_and_load()
    from gateway.run import GatewayRunner
    from gateway.config import load_gateway_config, Platform
    gateway = GatewayRunner(load_gateway_config())
    try:
        release_reservation(device_reservation)
        assert await asyncio.wait_for(gateway.start(), 60), 'Native Gateway did not start'
        adapter = gateway.adapters[Platform('kubik')]
        assert adapter._running
        from telegram_contract import check_telegram_callbacks
        await check_telegram_callbacks(adapter, gateway, providers)
        await check_profile_scopes(adapter, home)
        codec = importlib.import_module(adapter.__class__.__module__.rsplit('.', 1)[0] + '.codec')
        private_key = ec.generate_private_key(ec.SECP256R1())
        async with ClientSession() as client:
            ws = await pair(client, device_port, home, private_key)
            assert not providers.calls, providers.calls
            await pair(client, device_port, home, private_key, proof='wrong-binding')
            assert not providers.calls
            await pair(client, device_port, home, private_key, proof='invalid-signature')
            assert not providers.calls
            assert not ws.closed
            audio = await voice(ws, codec, 1, 'kubik-answer:first')
            assert len(audio) >= 4802, len(audio)
            assert {kind for kind, _ in providers.calls} == {'stt', 'llm', 'tts'}, providers.calls
            await ws.close()
            providers.transcript = 'kubik-second'
            ws = await pair(client, device_port, home, private_key, volume=0)
            before = len([1 for kind, _ in providers.calls if kind == 'tts'])
            audio = await voice(ws, codec, 2, 'kubik-answer:second')
            assert not audio
            assert len([1 for kind, _ in providers.calls if kind == 'tts']) == before
            history = next(body['messages'] for kind, body in reversed(providers.calls) if kind == 'llm')
            assert 'kubik-first' in json.dumps(history), history
            await ws.close()
            providers.transcript = 'kubik-independent'
            ws = await pair(client, device_port, home, ec.generate_private_key(ec.SECP256R1()), name='kubik-other', volume=0)
            await voice(ws, codec, 3, 'kubik-answer:first')
            separate = next(body['messages'] for kind, body in reversed(providers.calls) if kind == 'llm' and body.get('stream'))
            assert 'kubik-first' not in json.dumps(separate), separate
            security = importlib.import_module(adapter.__class__.__module__.rsplit('.', 1)[0] + '.security')
            security.atomic_json(adapter.security.allow_path, [])
            before = len(providers.calls)
            await ws.send_json({'t': 'ptt', 'on': True, 'turn': 4})
            denied = await get_json(ws)
            assert denied['t'] == 'error' and len(providers.calls) == before, denied
            await ws.close()
        print('PASS: native Gateway/AIAgent, signed WSS approval, rejected signatures/bindings, STT, context across reconnect, TTS/IMA acknowledgements, muted text, separate devices, revocation and profile scopes', flush=True)
    finally:
        release_reservation(backend_reservation)
        release_reservation(device_reservation)
        await asyncio.wait_for(gateway.stop(), 60)
        await backend.cleanup()
        print('Blocked non-loopback connection attempts:', len(BLOCKED), flush=True)


if __name__ == '__main__':
    logging.basicConfig(level=logging.WARNING)
    asyncio.run(main())
