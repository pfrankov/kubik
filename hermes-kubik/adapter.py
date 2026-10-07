"""Hermes platform: TLS and v5 device protocol live in the existing gateway process."""
import asyncio
import base64
import contextlib
import json
import os
from pathlib import Path
import secrets
import tempfile
import time
import wave

from aiohttp import WSMsgType, web
from gateway.config import Platform
from gateway.platforms.base import BasePlatformAdapter, SendResult
from gateway.platforms.event import MessageEvent, MessageType

from .codec import decode
from .security import Security


class Peer:
    def __init__(self, socket, identity, hello):
        self.socket, self.identity = socket, identity
        self.ready = False
        self.volume = hello.get('volume', 0)
        if type(self.volume) is not int or not 0 <= self.volume <= 100:
            raise ValueError('Invalid volume')
        self.turn = None
        self.pcm = bytearray()
        self.recorded_at = 0
        self.task = None
        self.gen = 0
        self.played_ms = 0
        self.progress = asyncio.Event()
        self.receipts = {}
        self.complete = asyncio.Event()
        self.message_id = None
        self.session_key = None
        self.input_turn = None
        self.stage = "idle"
        self.muted = False
        self.output_lock = asyncio.Lock()
        self.output_task = None
        self.sent_ms = 0
        self.waiting = False
        self.pending_previous = None

    async def send(self, message_type, **fields):
        await self.socket.send_json({'t': message_type, **fields})


class KubikAdapter(BasePlatformAdapter):
    def __init__(self, config):
        super().__init__(config, Platform('kubik'))
        self.security = Security(os.environ.get('HERMES_HOME', str(Path.home() / '.hermes')))
        self.peers = {}
        self.sockets = set()
        self.runner = None
        self.transcriptions = set()

    async def connect(self, *, is_reconnect=False):
        app = web.Application(client_max_size=4096)
        app.router.add_get('/kubik/v1', self.accept)
        self.runner = web.AppRunner(app, access_log=None, handler_cancellation=True)
        await self.runner.setup()
        extra = self.config.extra or {}
        port = int(os.environ.get('KUBIK_PORT', extra.get('port', 18793)))
        if not 1024 <= port <= 65535:
            raise ValueError('Invalid Kubik port')
        await web.TCPSite(self.runner, extra.get('bind', '0.0.0.0'), port,
                          ssl_context=self.security.tls(), backlog=32).start()
        self._running = True
        return True

    async def disconnect(self):
        self._running = False
        for socket in tuple(self.sockets):
            await socket.close(code=1001)
        if self.runner:
            await self.runner.cleanup()

    async def receive_json(self, socket):
        message = await socket.receive(timeout=8)
        if message.type != WSMsgType.TEXT:
            raise ValueError('Expected JSON')
        value = json.loads(message.data)
        if not isinstance(value, dict):
            raise ValueError('Expected object')
        return value

    async def authenticate(self, socket):
        hello = await self.receive_json(socket)
        self.security.identity(hello)
        nonce = base64.b64encode(secrets.token_bytes(32)).decode()
        await socket.send_json({'t': 'challenge', 'nonce': nonce})
        auth = await self.receive_json(socket)
        if auth.get('t') != 'auth':
            raise ValueError('Expected auth')
        identity = self.security.verify(hello, nonce, auth.get('sig'))
        if not self.security.approved(identity):
            code = self.security.pair(identity)
            await socket.send_json({'t': 'pair', 'code': code})
            await self.wait_pair(socket, identity)
        return Peer(socket, identity, hello)

    async def wait_pair(self, socket, identity):
        deadline = time.monotonic() + 600
        while not self.security.approved(identity):
            if time.monotonic() >= deadline:
                raise TimeoutError('Pairing expired')
            try:
                message = await socket.receive(timeout=2)
            except asyncio.TimeoutError:
                continue
            if message.type != WSMsgType.TEXT:
                raise ValueError('Pairing closed')
            value = json.loads(message.data)
            if value.get('t') != 'ping':
                raise ValueError('Pairing only accepts ping')
            await socket.send_json({'t': 'pong', 'ts': value.get('ts')})

    def speech_available(self):
        from tools.transcription_tools import is_stt_enabled, _load_stt_config, _get_provider
        from tools.tts_tool import check_tts_requirements
        config = _load_stt_config()
        return {'available': bool(is_stt_enabled(config) and _get_provider(config) != 'none')}, {'available': bool(check_tts_requirements())}

    async def accept(self, request):
        if len(self.sockets) >= 8:
            return web.Response(status=503)
        socket = web.WebSocketResponse(max_msg_size=4096, heartbeat=20)
        self.sockets.add(socket)
        peer = None
        try:
            await socket.prepare(request)
            peer = await self.authenticate(socket)
            old = self.peers.get(peer.identity)
            # Publish the replacement before close yields to another handshake.
            self.peers[peer.identity] = peer
            if old:
                await old.socket.close(code=4003)
            if self.peers.get(peer.identity) is not peer or socket.closed:
                return socket
            await peer.send('welcome', session=secrets.token_hex(8), progress=True)
            stt, tts = self.speech_available()
            await peer.send('capabilities', stt=stt, tts=tts, voice_mode='classic')
            peer.ready = self.peers.get(peer.identity) is peer and not socket.closed
            async for message in socket:
                if message.type == WSMsgType.TEXT:
                    await self.command(peer, json.loads(message.data))
                elif message.type == WSMsgType.BINARY:
                    self.microphone(peer, message.data)
                elif message.type in (WSMsgType.CLOSE, WSMsgType.ERROR):
                    break
        except asyncio.CancelledError:
            raise
        except (ValueError, TypeError, KeyError, TimeoutError):
            await socket.close(code=4002)
        except Exception:
            # No payloads, transcripts or cryptographic material in the log/close reason.
            await socket.close(code=4001)
        finally:
            self.sockets.discard(socket)
            if peer:
                peer.ready = False
                if self.peers.get(peer.identity) is peer:
                    del self.peers[peer.identity]
                await self.cancel(peer)
        return socket

    def microphone(self, peer, frame):
        if len(frame) < 6 or frame[0] != 4 or peer.turn != frame[1]:
            raise ValueError('Invalid microphone turn')
        if len(peer.pcm) + (len(frame) - 5) * 4 > 60 * 24000 * 2:
            raise ValueError('Recording too long')
        peer.pcm.extend(decode(frame[2:]))

    async def cancel(self, peer):
        previous = peer.pending_previous
        peer.input_turn = None
        peer.turn = None
        peer.pcm.clear()
        if peer.task and not peer.task.done():
            peer.task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await peer.task
        peer.task = previous if previous and not previous.done() else None
        peer.waiting = False
        peer.pending_previous = None
        peer.progress.set()
        if peer.socket.closed:
            for receipt in peer.receipts.values():
                receipt.set()

    async def command(self, peer, value):
        if not isinstance(value, dict):
            raise ValueError('Expected object')
        kind = value.get('t')
        if kind == 'ping':
            await peer.send('pong', ts=value.get('ts'))
        elif kind == 'device_state':
            volume = value.get('volume')
            if type(volume) is int and 0 <= volume <= 100:
                peer.volume = volume
        elif kind == 'ptt':
            await self.ptt(peer, value)
        elif kind == 'cancel':
            await self.cancel_request(peer, value)
        elif kind == 'progress' and value.get('gen') == peer.gen:
            ms = value.get('ms')
            if isinstance(ms, int) and peer.played_ms <= ms <= 120000:
                peer.played_ms = ms
                peer.progress.set()
        elif kind in ('played', 'shown'):
            key = (kind, value.get('gen') if kind == 'played' else value.get('receipt'))
            full = kind == 'shown' or (type(value.get('ms')) is int and value['ms'] >= peer.sent_ms)
            if full and key in peer.receipts:
                peer.receipts[key].set()
        elif kind in ('agent_options', 'agent_model'):
            await peer.send('agent_options', target=value.get('target'), rid=value.get('rid'),
                            cursor=value.get('cursor', 0), error='unsupported')

    async def ptt(self, peer, value):
        turn = value.get('turn')
        if not isinstance(turn, int) or not 0 <= turn <= 255 or type(value.get('on')) is not bool:
            raise ValueError('Invalid PTT')
        if value['on']:
            running = peer.task and not peer.task.done()
            if peer.turn is not None or peer.waiting or (running and not peer.muted):
                await peer.send('error', code='busy')
                return
            peer.turn, peer.recorded_at = turn, time.monotonic()
            peer.input_turn = turn
            if not running:
                peer.muted = False
            peer.pcm.clear()
        elif peer.turn == turn:
            pcm = bytes(peer.pcm)
            peer.turn = None
            peer.pcm.clear()
            if pcm:
                peer.pending_previous = peer.task
                peer.waiting = bool(peer.task and not peer.task.done())
                peer.task = asyncio.create_task(self.dispatch_after(peer, pcm, peer.task))

    async def cancel_request(self, peer, value):
        if 'turn' in value:
            if value['turn'] == peer.turn:
                peer.turn = None
                peer.pcm.clear()
                peer.input_turn = None
            elif value['turn'] == peer.input_turn and (peer.waiting or peer.stage != 'agent'):
                await self.cancel(peer)
            return
        if 'gen' in value and value['gen'] != peer.gen:
            return
        if peer.output_task and not peer.output_task.done():
            peer.muted = True
            peer.output_task.cancel()
            await peer.send('speak_cancel', gen=peer.gen)
        if peer.stage == 'agent':
            peer.muted = True
            if peer.output_task and not peer.output_task.done():
                peer.output_task.cancel()
            await peer.send('speak_cancel', gen=peer.gen)
        else:
            await self.cancel(peer)
        await peer.send('state', s='idle')

    async def dispatch_after(self, peer, pcm, previous):
        input_turn = peer.input_turn
        try:
            if previous and not previous.done():
                async with asyncio.timeout(240):
                    await asyncio.shield(previous)
            peer.waiting = False
            peer.pending_previous = None
            peer.muted = False
            await self.dispatch(peer, pcm)
        except TimeoutError:
            await peer.send('error', code='busy')
        finally:
            peer.waiting = False
            peer.pending_previous = None
            if peer.input_turn == input_turn:
                peer.input_turn = None
            if previous and not previous.done() and peer.task is asyncio.current_task():
                peer.task = previous
            if not previous or previous.done():
                peer.stage = 'idle'
                peer.muted = False

    async def dispatch(self, peer, pcm):
        from tools.transcription_tools import transcribe_audio
        input_turn = peer.input_turn
        session_key = None
        path = None
        transcription = None
        try:
            if len(self.transcriptions) >= 2:
                await peer.send('error', code='busy')
                return
            peer.stage = 'transcribing'
            await peer.send('state', s='transcribing')
            fd, path = tempfile.mkstemp(prefix='kubik-', suffix='.wav')
            os.close(fd)
            with wave.open(path, 'wb') as stream:
                stream.setparams((1, 2, 24000, 0, 'NONE', 'not compressed'))
                stream.writeframes(pcm)
            transcription = asyncio.create_task(asyncio.to_thread(transcribe_audio, path, source='gateway'))
            self.transcriptions.add(transcription)
            transcription.add_done_callback(lambda task: self.finish_transcription(task, path))
            async with asyncio.timeout(120):
                result = await asyncio.shield(transcription)
            text = result.get('transcript', '') if result.get('success') else ''
            if not isinstance(text, str) or not text.strip() or len(text) > 16000:
                await peer.send('error', code='stt_empty' if result.get('success') else 'stt_failed')
                return
            # This device's request enters the normal gateway memory/tool/voice-reply pipeline.
            event = MessageEvent(text=text, message_type=MessageType.VOICE,
                source=self.build_source(peer.identity, chat_name='Kubik', user_id=peer.identity,
                                         role_authorized=self.security.approved(peer.identity)),
                user_id=peer.identity, message_id=secrets.token_hex(8))
            peer.complete.clear()
            peer.message_id = event.message_id
            session_key = self._event_session_key(event)
            peer.session_key = session_key
            peer.stage = 'agent'
            async with asyncio.timeout(240):
                await self.handle_message(event)
                if not event._gateway_accepted:
                    raise ValueError('Hermes rejected voice event')
                await peer.complete.wait()
        except asyncio.CancelledError:
            raise
        except Exception:
            if session_key:
                await self.cancel_session_processing(session_key, discard_pending=True)
            if not peer.socket.closed:
                await peer.send('error', code='agent_failed')
        finally:
            if path and (transcription is None or transcription.done()):
                Path(path).unlink(missing_ok=True)
            peer.stage = 'idle'
            if peer.input_turn == input_turn:
                peer.input_turn = None
            peer.muted = False
            if not peer.socket.closed:
                await peer.send('state', s='idle')

    def finish_transcription(self, task, path):
        self.transcriptions.discard(task)
        Path(path).unlink(missing_ok=True)
        if not task.cancelled():
            task.exception()  # consume a late provider failure after timeout/cancellation

    async def on_processing_complete(self, event, outcome):
        peer = self.peers.get(event.source.chat_id)
        if peer and peer.message_id == event.message_id:
            peer.complete.set()

    def _is_sender_authorized(self, user_id, chat_type=None, **kwargs):
        # A normal gateway session can only be created by this adapter after v5 proof and local approval.
        return isinstance(user_id, str) and self.security.approved(user_id)

    def _ready_peer(self, chat_id):
        peer = self.peers.get(chat_id)
        return peer if peer and peer.ready and not peer.socket.closed else None

    def _should_auto_tts_for_chat(self, chat_id):
        peer = self._ready_peer(chat_id)
        return bool(peer and type(peer.volume) is int and peer.volume >= 20)

    async def send_typing(self, chat_id, **kwargs):
        peer = self._ready_peer(chat_id)
        if peer and not peer.muted:
            await peer.send('state', s='thinking')

    async def send(self, chat_id, content, reply_to=None, metadata=None, **kwargs):
        peer = self._ready_peer(chat_id)
        if not peer:
            return SendResult(success=False, error='Kubik disconnected', retryable=True)
        if peer.muted:
            return SendResult(success=True, message_id='cancelled')
        receipt = secrets.randbelow(0xffffffff) + 1
        done = asyncio.Event()
        peer.receipts[('shown', receipt)] = done
        try:
            await peer.send('text', text=str(content).encode()[:1000].decode('utf-8', errors='ignore'),
                            kind='reply' if peer.task and not peer.task.done() else 'notify', receipt=receipt)
            await asyncio.wait_for(done.wait(), 10)
            if peer.socket.closed:
                raise ConnectionError('Disconnected before receipt')
            return SendResult(success=True, message_id=str(receipt))
        except (TimeoutError, ConnectionError):
            return SendResult(success=False, error='Kubik did not acknowledge', retryable=True)
        finally:
            peer.receipts.pop(('shown', receipt), None)

    async def send_image(self, chat_id, image_url, caption=None, **kwargs):
        return SendResult(success=False, error='Kubik supports text and voice')

    async def get_chat_info(self, chat_id):
        return {'name': 'Kubik', 'type': 'dm', 'chat_id': chat_id}

    async def send_voice(self, chat_id, audio_path, caption=None, **kwargs):
        from .speech import play_file
        peer = self._ready_peer(chat_id)
        if not peer:
            return SendResult(success=False, error='Kubik disconnected', retryable=True)
        async with peer.output_lock:
            if self._ready_peer(chat_id) is not peer:
                return SendResult(success=False, error='Kubik disconnected', retryable=True)
            if peer.muted or peer.volume < 20:
                return SendResult(success=True, message_id='muted')
            peer.output_task = asyncio.create_task(play_file(peer, audio_path))
            try:
                return await peer.output_task
            except asyncio.CancelledError:
                if peer.muted:
                    return SendResult(success=True, message_id='cancelled')
                raise
            finally:
                peer.output_task = None

    async def play_tts(self, chat_id, audio_path, **kwargs):
        return await self.send_voice(chat_id, audio_path, **kwargs)
