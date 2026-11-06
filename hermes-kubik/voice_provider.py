"""Bounded, server-owned OpenAI voice transports; no credentials reach Kubik."""
import asyncio
import base64
import binascii
from dataclasses import dataclass, field
from urllib.parse import urlencode, urlsplit, urlunsplit
from uuid import uuid4

import aiohttp

PCM_RATE = 24000
MAX_AUDIO = PCM_RATE * 2 * 5
MAX_TEXT = 8192
MAX_BLOCKS = 512


class ProviderError(RuntimeError):
    """Only stable local codes may cross the provider boundary."""


@dataclass(frozen=True)
class ProviderSettings:
    api_key: str = field(repr=False)
    base_url: str = 'https://api.openai.com/v1'
    model: str = 'gpt-realtime-2.1'
    voice: str = 'marin'
    instructions: str = ''


@dataclass(frozen=True)
class ProviderLimits:
    connect: float = 10
    response: float = 120
    session: float = 1800
    close: float = 2


def identifier(value):
    if not isinstance(value, str) or not value or len(value) > 512:
        raise ProviderError('invalid_provider_id')
    return value


def bounded_text(value):
    if not isinstance(value, str) or len(value) > MAX_TEXT:
        raise ProviderError('provider_text_limit')
    return value


def pcm_decode(value):
    if not isinstance(value, str) or len(value) > MAX_AUDIO * 4 // 3 + 4:
        raise ProviderError('provider_audio_limit')
    try:
        pcm = base64.b64decode(value, validate=True)
    except (ValueError, binascii.Error):
        raise ProviderError('invalid_provider_audio') from None
    validate_pcm(pcm)
    return pcm


def validate_pcm(pcm):
    if not isinstance(pcm, bytes) or len(pcm) % 2 or len(pcm) > MAX_AUDIO:
        raise ProviderError('invalid_provider_audio')


def endpoint_url(settings, mode):
    parsed = urlsplit(settings.base_url)
    if parsed.scheme not in ('https', 'wss') or not parsed.hostname:
        raise ProviderError('invalid_provider_endpoint')
    if parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ProviderError('invalid_provider_endpoint')
    suffix = '/live/sessions' if mode == 'live' else '/realtime'
    query = '' if mode == 'live' else urlencode({'model': settings.model})
    return urlunsplit(('wss', parsed.netloc, parsed.path.rstrip('/') + suffix, query, ''))


class VoiceProvider:
    """One connection, one immutable generation. Callbacks must remain short."""
    mode = ''
    input_event = ''

    def __init__(self, settings, on_event, *, endpoint=None, limits=None):
        self.settings, self.on_event = settings, on_event
        self.limits = limits or ProviderLimits()
        self.endpoint = endpoint or endpoint_url(settings, self.mode)
        self.http = self.ws = self.reader = self.timer = None
        self.ready = asyncio.Event()
        self.closed = asyncio.Event()
        self.started = self.stopping = False
        self.failure = None
        self.epoch = 0
        self._write_lock = asyncio.Lock()
        self._input_bytes = 0
        self._cleanup_task = None

    async def start(self):
        if self.started or self.stopping:
            raise ProviderError('provider_already_started')
        self.started = True
        if not self.settings.api_key:
            raise ProviderError('provider_key_missing')
        try:
            async with asyncio.timeout(self.limits.connect):
                self.http = aiohttp.ClientSession(
                    timeout=aiohttp.ClientTimeout(total=None, sock_connect=self.limits.connect))
                self.ws = await self.http.ws_connect(
                    self.endpoint, headers={'Authorization': 'Bearer ' + self.settings.api_key},
                    max_msg_size=MAX_AUDIO * 2, heartbeat=20)
                self.reader = asyncio.create_task(self._receive())
                await self._configure()
                await self.ready.wait()
                self.check_open()
            self.timer = asyncio.create_task(self._expire())
        except (TimeoutError, aiohttp.ClientError, OSError):
            await self.fail('provider_connect_failed')
            await self.close()
            raise ProviderError('provider_connect_failed') from None
        except BaseException:
            await self.close()
            raise

    def check_open(self):
        if self.failure or self.stopping or self.ws is None or self.ws.closed:
            raise ProviderError(self.failure or 'provider_closed')

    async def emit(self, kind, data):
        if not self.stopping:
            await self.on_event(kind, data)

    async def send(self, event, *, before_send=None):
        self.check_open()
        event.setdefault('event_id', uuid4().hex)
        try:
            async with asyncio.timeout(self.limits.connect):
                async with self._write_lock:
                    self.check_open()
                    if before_send:
                        before_send(event)
                    await self.ws.send_json(event)
        except (TimeoutError, aiohttp.ClientError, OSError):
            await self.fail('provider_write_failed')
            raise ProviderError('provider_write_failed') from None
        return event['event_id']

    async def append(self, pcm):
        validate_pcm(pcm)
        self.check_open()
        if not self.ready.is_set():
            raise ProviderError('provider_not_ready')
        self._input_bytes += len(pcm)
        try:
            if self._input_bytes > MAX_AUDIO:
                raise ProviderError('provider_input_overflow')
            if pcm:
                await self.send({'type': self.input_event,
                                 'audio': base64.b64encode(pcm).decode('ascii')})
        finally:
            self._input_bytes -= len(pcm)

    async def _receive(self):
        try:
            async for message in self.ws:
                event = self._parse(message)
                if event['type'] == 'session.closed':
                    self.closed.set()
                    break
                if self.stopping:
                    continue
                await self.handle(event)
            if not self.stopping:
                await self.fail('provider_connection_closed')
        except asyncio.CancelledError:
            raise
        except Exception as error:
            code = str(error) if isinstance(error, ProviderError) else 'provider_protocol_error'
            await self.fail(code)
        finally:
            self.ready.set()

    def _parse(self, message):
        if message.type != aiohttp.WSMsgType.TEXT:
            raise ProviderError('invalid_provider_message')
        event = message.json()
        if not isinstance(event, dict) or not isinstance(event.get('type'), str):
            raise ProviderError('invalid_provider_message')
        if event['type'] == 'error':
            raise ProviderError('provider_rejected_request')
        startup = ('session.created', 'session.updated', 'session.started', 'session.closed')
        if not self.ready.is_set() and event['type'] not in startup:
            raise ProviderError('provider_event_before_ready')
        return event

    async def fail(self, code):
        if self.failure or self.stopping:
            return
        self.failure = code
        self.ready.set()
        await self.emit('error', {'code': code})
        await self.close()

    async def _expire(self):
        await asyncio.sleep(self.limits.session)
        await self.fail('provider_session_timeout')
        await self.close()

    async def close(self):
        if self._cleanup_task is None:
            self.stopping = True
            self.epoch += 1
            self._cleanup_task = asyncio.create_task(self._shutdown(asyncio.current_task()))
        await asyncio.shield(self._cleanup_task)

    async def _shutdown(self, origin):
        await self._close_session(origin)
        for task in (self.reader, self.timer):
            if task and task is not origin:
                task.cancel()
        if self.ws:
            try:
                async with asyncio.timeout(self.limits.close):
                    await self.ws.close()
            except (TimeoutError, aiohttp.ClientError, OSError):
                pass
        if self.http:
            await self.http.close()
        for task in (self.reader, self.timer):
            if task and task is not origin:
                await asyncio.gather(task, return_exceptions=True)
        self.closed.set()

    async def _close_session(self, current):
        if self.mode != 'live' or self.ws is None or self.ws.closed:
            return
        try:
            async with asyncio.timeout(self.limits.close):
                await self.ws.send_json({'type': 'session.close', 'event_id': uuid4().hex})
                if self.reader is not current:
                    await self.closed.wait()
        except (TimeoutError, aiohttp.ClientError, OSError):
            pass


def create_provider(mode, settings, on_event, **kwargs):
    if mode == 'realtime':
        from .realtime_provider import RealtimeProvider
        return RealtimeProvider(settings, on_event, **kwargs)
    if mode == 'live':
        from .live_provider import LiveProvider
        return LiveProvider(settings, on_event, **kwargs)
    raise ProviderError('unsupported_voice_mode')
