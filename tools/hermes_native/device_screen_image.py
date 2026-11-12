"""Physical image attachment delivery; exactly one display and no printer commands."""
import asyncio
import importlib
import hashlib
import hmac
import io
import os
import stat
import time
import json
from pathlib import Path

from device_control import fault_window, save


class Observations:
    def __init__(self, adapter):
        self.adapter = adapter
        self.command, self.authenticate = adapter.command, adapter.authenticate
        self.senders, self.evidence = [], None
        self.frames = self.bytes = self.shows = 0
        self.acks = []
        self.rid = None

    def install(self):
        async def authenticate(socket):
            peer = await self.authenticate(socket)
            self.watch_socket(socket)
            return peer
        async def command(peer, value):
            if value.get('t') == 'screen_image_result':
                self.acks.append({key: value.get(key) for key in ('rid', 'status', 'code') if key in value})
                self.persist_current()
            return await self.command(peer, value)
        self.adapter.authenticate, self.adapter.command = authenticate, command

    def watch_socket(self, socket):
        if any(item[0] is socket for item in self.senders):
            return
        send_json, send_bytes = socket.send_json, socket.send_bytes
        self.senders.append((socket, send_json, send_bytes))
        async def guarded_json(value, **kwargs):
            kind = value.get('t')
            if kind in ('printer', 'printer_image'):
                raise AssertionError('Screen image fixture forbids every printer command')
            if kind == 'screen_image' and value.get('action') == 'show':
                if self.shows or self.bytes != 8224 or value.get('rid') != self.rid:
                    raise AssertionError('Unexpected or repeated image show')
                self.shows += 1
                self.persist_current()
            return await send_json(value, **kwargs)
        async def guarded_bytes(value):
            if value and value[0] == 6:
                raise AssertionError('Screen image fixture forbids printer raster')
            if value and value[0] == 7:
                self.record_preview(value)
            return await send_bytes(value)
        socket.send_json, socket.send_bytes = guarded_json, guarded_bytes

    def record_preview(self, value):
        if not 37 <= len(value) <= 932 or value[1] != 1 or self.shows:
            raise AssertionError('Unexpected preview packet')
        rid = value[2:34].decode('ascii')
        offset = int.from_bytes(value[34:36], 'big')
        if (any(c not in '0123456789abcdef' for c in rid) or
                (self.rid is not None and self.rid != rid) or offset != self.bytes):
            raise AssertionError('Image packet identity or order changed')
        self.rid = rid
        self.bytes += len(value) - 36
        self.frames += 1
        if self.bytes > 8224 or self.frames > 10:
            raise AssertionError('Screen image fixture transfer exceeded budget')

    def persist_current(self):
        if self.evidence:
            save(self.evidence / 'screen-image-wire.json', {'frames': self.frames, 'bytes': self.bytes,
                 'shows': self.shows, 'acks': self.acks, 'printer_jobs': 0})

    def persist(self, evidence, providers):
        self.evidence = evidence
        self.persist_current()

    def restore(self):
        self.adapter.command, self.adapter.authenticate = self.command, self.authenticate
        for socket, send_json, send_bytes in self.senders:
            socket.send_json, socket.send_bytes = send_json, send_bytes


def source_bytes(path, expected_sha, expected_owner, peer):
    if expected_owner != peer.identity:
        raise ValueError('Verified image owner differs from the current paired device')
    if not isinstance(expected_sha, str) or len(expected_sha) != 64:
        raise ValueError('Invalid verified source digest')
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        meta = os.fstat(descriptor)
        if not stat.S_ISREG(meta.st_mode) or not 1 <= meta.st_size <= 8 * 1024 * 1024:
            raise ValueError('Invalid verified image size or file type')
        with os.fdopen(descriptor, 'rb', closefd=False) as file:
            data = file.read(8 * 1024 * 1024 + 1)
        if len(data) != meta.st_size or not hmac.compare_digest(hashlib.sha256(data).hexdigest(), expected_sha):
            raise ValueError('Verified image changed since the owner check')
    finally:
        os.close(descriptor)
    verify_png(data)
    return data


def verify_png(data):
    from PIL import Image
    with Image.open(io.BytesIO(data)) as image:
        width, height = image.size
        if image.format != 'PNG' or getattr(image, 'n_frames', 1) != 1 or not 1 <= width <= 4096 or not 1 <= height <= 4096 or width * height > 4 * 1024 * 1024:
            raise ValueError('Verified source is not a supported PNG')
        image.load()


def fixture_png(path, source, peer):
    if source:
        data = source_bytes(*source, peer)
        path.write_bytes(data)
        return {'image': 'verified existing attachment', 'source_sha256': hashlib.sha256(data).hexdigest(),
                'source_size': len(data)}
    from PIL import Image, ImageDraw
    image = Image.new('RGB', (160, 120), '#1054df')
    draw = ImageDraw.Draw(image)
    draw.rectangle((15, 15, 145, 105), fill='#fa7a20')
    draw.ellipse((55, 20, 105, 100), fill='#2af0ae')
    image.save(path)
    return {'image': 'synthetic local fixture'}


def verify_baseline(baseline, live):
    assert not baseline['printer_busy']
    if live:
        assert baseline['live_active'] and baseline['mic_open'] and baseline['mic_rx_enabled']
    else:
        assert not baseline['live_active'] and not baseline['mic_open']


async def baseline_when_printer_idle(control, live):
    state = await control.until(lambda item: item.get('printer_busy') is False,
                                'printer background work finished', timeout=40)
    verify_baseline(state, live)
    return state


async def check(adapter, peer, control, providers, observations, *, live=False, baseline=None):
    from gateway.session_context import set_session_vars, clear_session_vars
    from hermes_constants import set_hermes_home_override, reset_hermes_home_override
    from image_contract import transform
    observations.persist(control.evidence, providers)
    assert peer.screen_image_version == 1, 'Firmware lacks screen image capability'
    before_faults = await control.request(cmd='bridge-diagnostics')
    if baseline is None:
        baseline = await baseline_when_printer_idle(control, live)
    else:
        verify_baseline(baseline, live)
    module = importlib.import_module(type(adapter).__module__.rsplit('.', 1)[0] + '.printer_tool')
    event = adapter._authorized_voice_event(peer, 'screen-image-check')
    assert event is not None
    home = Path(adapter.home)
    directory = home / 'cache/generated/images'
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / 'physical-screen-fixture.png'
    image_evidence = fixture_png(path, getattr(control, 'screen_image_source', None), peer)
    raw = json.dumps({'success': True, 'provider': 'openai', 'image': str(path)})
    tokens = set_session_vars(platform='kubik', chat_id=peer.identity, message_id=event.message_id,
                              session_key=adapter._event_session_key(event))
    profile = set_hermes_home_override(home)
    try:
        with module.printer_origin(adapter, peer, event):
            captured = json.loads(await asyncio.to_thread(transform, raw))
        assert captured.get('kubik_image_ref'), 'Image-generation result not bound to this session'
        await adapter.on_processing_complete(event, 'success')
        event = None
        denied = await adapter.send_image_file(peer.identity, str(directory / 'unowned.png'))
        assert not denied.success and observations.frames == observations.shows == 0
        delivered = await adapter.send_image_file(peer.identity, str(path))
        assert delivered.success, delivered.error
        assert observations.frames == 10 and observations.bytes == 8224 and observations.shows == 1
        assert len(observations.acks) == 1 and observations.acks[0]['status'] == 'shown'
        state = await control.info()
        assert state['printer_preview_active'] and state['printer_preview_presented']
        assert not state['printer_busy'] and state['online'] and state['via'] == ('wifi' if baseline['via'] == 'wifi' else 'usb')
        assert state['heap_failures'] == baseline['heap_failures'], 'Heap allocation failed'
        fault_window(before_faults, await control.request(cmd='bridge-diagnostics'))
        observations.persist_current()
        return {'attachment_shown': True, 'shown_ack': True, 'preview_presented': True,
                'printer_jobs': 0, 'paid_provider_calls': 0, **image_evidence}
    finally:
        if event is not None:
            await adapter.on_processing_complete(event, 'success')
        reset_hermes_home_override(profile)
        clear_session_vars(tokens)
        path.unlink(missing_ok=True)


class LiveObservations(Observations):
    def __init__(self, adapter):
        super().__init__(adapter)
        self.states, self.errors, self.connection = [], [], None

    def persist(self, evidence, providers):
        super().persist(evidence, providers)
        save(evidence / 'screen-image-live-states.json', self.states)
        save(evidence / 'screen-image-live-errors.json', self.errors)
        save(evidence / 'screen-image-live-mic-times.json', self.connection.mic_times if self.connection else [])


async def sample_live(control, observations, stopped):
    fields = ('heap_failures', 'heap_internal', 'heap_dma', 'printer_busy', 'printer_preview_active',
              'printer_preview_presented', 'live_active', 'mic_open', 'mic_rx_enabled', 'online')
    try:
        while not stopped.is_set():
            state = await control.info()
            assert all(field in state for field in fields), 'Live screen image diagnostics incomplete'
            if len(observations.states) >= 256:
                raise AssertionError('Live screen image observation limit')
            observations.states.append({'at': time.monotonic(), **{key: state[key] for key in fields}})
            await asyncio.sleep(.3)
    except Exception as error:
        observations.errors.append({'type': type(error).__name__, 'message': str(error)[:200]})


def active_row(row):
    return (row['printer_preview_active'] and row['printer_preview_presented'] and row['live_active'] and
            row['mic_open'] and row['mic_rx_enabled'] and row['online'] and not row['printer_busy'])


def overlap(observations):
    rows = [row for row in observations.states if active_row(row)]
    if len(rows) < 2 or observations.connection is None:
        return []
    return [stamp for stamp in observations.connection.mic_times if rows[0]['at'] < stamp < rows[-1]['at']]


def verify_live(observations, baseline):
    assert not observations.errors, observations.errors
    assert observations.states and all(row['heap_failures'] == baseline['heap_failures'] for row in observations.states), 'Heap allocation failed'
    shown = [row for row in observations.states if row['printer_preview_active'] and row['printer_preview_presented']]
    assert shown and all(active_row(row) for row in shown), 'Live microphone or connection stopped during preview'
    frames = overlap(observations)
    assert frames, 'No microphone frames observed during displayed Live image'
    return {'continuous_microphone': True, 'mic_frames_during_preview': len(frames),
            'preview_samples': sum(active_row(row) for row in observations.states),
            'heap_internal_min': min(row['heap_internal'] for row in observations.states)}


async def check_live(adapter, peer, control, providers, observations):
    from device_printer import MicConnection
    from device_voice import begin
    from voice_fixture import VoiceFixture, eventually
    before = await control.request(cmd='bridge-diagnostics')
    baseline = await control.info()
    providers.voice = VoiceFixture(MicConnection)
    old_volume, stopped, monitor = peer.volume, asyncio.Event(), None
    peer.volume = 0
    try:
        observations.connection = await begin('live', adapter, peer, control, providers.voice)
        await control.sim('ptt_up')
        live_baseline = await baseline_when_printer_idle(control, True)
        monitor = asyncio.create_task(sample_live(control, observations, stopped))
        result = await check(adapter, peer, control, providers, observations, live=True, baseline=live_baseline)
        await eventually(lambda: bool(overlap(observations)) or bool(observations.errors), timeout=8)
        stopped.set()
        await monitor
        monitor = None
        assert peer.native and peer.native.current(), 'Live closed while displaying image'
        measured = verify_live(observations, baseline)
        fault_window(before, await control.request(cmd='bridge-diagnostics'))
        return {**result, **measured}
    finally:
        stopped.set()
        if monitor:
            await monitor
        peer.volume = old_volume
        observations.persist(control.evidence, providers)
