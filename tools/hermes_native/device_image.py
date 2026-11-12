"""Exactly one synthetic image through the real Hermes hook and physical C6."""
import asyncio
import importlib
import json
from pathlib import Path

from device_control import save


class Observations:
    def __init__(self, adapter):
        self.adapter = adapter
        self.senders = []
        self.starts = 0
        self.bands = 0
        self.results = []
        self.evidence = None
        self.command = adapter.command

    def install(self):
        async def command(peer, value):
            if value.get('t') == 'printer_result':
                self.results.append(value)
                self.persist_current()
            return await self.command(peer, value)
        self.adapter.command = command

    def watch_socket(self, socket):
        if any(item[0] is socket for item in self.senders):
            return
        send_json, send_bytes = socket.send_json, socket.send_bytes
        self.senders.append((socket, send_json, send_bytes))
        async def guarded_json(value, **kwargs):
            if value.get('t') == 'printer_image' and value.get('action') == 'start':
                if self.starts:
                    raise AssertionError('Refusing a second physical image job')
                self.starts += 1
                self.persist_current()  # Record before dispatch; never replay an uncertain print.
            if value.get('t') == 'printer' and value.get('action') != 'status':
                raise AssertionError('Unexpected physical printer job')
            return await send_json(value, **kwargs)
        async def guarded_bytes(value):
            if value and value[0] == 6:
                if self.starts != 1:
                    raise AssertionError('Image data without a single approved start')
                self.bands += 1
            return await send_bytes(value)
        socket.send_json, socket.send_bytes = guarded_json, guarded_bytes

    def persist_current(self):
        if self.evidence:
            save(self.evidence / 'image-wire.json', {'starts': self.starts, 'bands': self.bands,
                                                       'results': self.results})

    def persist(self, evidence, providers):
        self.evidence = evidence
        self.persist_current()

    def restore(self):
        self.adapter.command = self.command
        for socket, send_json, send_bytes in self.senders:
            socket.send_json, socket.send_bytes = send_json, send_bytes


async def observe_preview(control, stopped, evidence):
    while not stopped.is_set():
        state = await control.info()
        evidence['preview_active'] |= state.get('printer_preview_active') is True
        evidence['preview_presented'] |= state.get('printer_preview_presented') is True
        await asyncio.sleep(.15)


async def check(adapter, peer, control, providers, observations, *, document=False, external_preview=None):
    from gateway.session_context import set_session_vars, clear_session_vars
    from hermes_constants import set_hermes_home_override, reset_hermes_home_override
    from model_tools import handle_function_call
    from image_contract import transform
    from PIL import Image, ImageDraw
    observations.persist(control.evidence, providers)
    await control.until(lambda state: not state['printer_busy'], 'printer background work finished', timeout=40)
    assert peer.printer_image_version == 1
    module = importlib.import_module(type(adapter).__module__.rsplit('.', 1)[0] + '.printer_tool')
    event = adapter._authorized_voice_event(peer, 'physical-image-check')
    home = Path(adapter.home)
    path = home / 'cache/generated/images/physical-fixture.png'
    path.parent.mkdir(parents=True, exist_ok=True)
    image = Image.new('RGB', (160, 160), 'white')
    drawing = ImageDraw.Draw(image)
    drawing.rectangle((8, 8, 151, 151), outline='black', width=4)
    drawing.ellipse((40, 40, 120, 120), outline='black', width=4)
    image.save(path)
    raw = json.dumps({'success': True, 'provider': 'openai', 'image': str(path)})
    tokens = set_session_vars(platform='kubik', chat_id=peer.identity, message_id=event.message_id,
                              session_key=adapter._event_session_key(event))
    profile = set_hermes_home_override(home)
    try:
        with module.printer_origin(adapter, peer, event):
            result = json.loads(await asyncio.to_thread(transform, raw))
            assert observations.starts == 0
        await adapter.on_processing_complete(event, 'success')
        clear_session_vars(tokens)
        event = adapter._authorized_voice_event(peer, 'physical-image-follow-up')
        tokens = set_session_vars(platform='kubik', chat_id=peer.identity, message_id=event.message_id,
                                  session_key=adapter._event_session_key(event))
        with module.printer_origin(adapter, peer, event):
            ref = result['kubik_image_ref']
            args = ({'action': 'print', 'format': 'document', 'width_mm': 30, 'blocks': [
                {'type': 'text', 'text': 'Текст перед рисунком'}, {'type': 'image', 'image_ref': ref, 'width_mm': 20},
                {'type': 'text', 'text': 'Текст после рисунка'}]} if document else
                {'action': 'print', 'format': 'image', 'image_ref': ref, 'width_mm': 20, 'height_mm': 20})
            evidence = external_preview if external_preview is not None else {'preview_active': False, 'preview_presented': False}
            stopped = asyncio.Event()
            monitor = None if external_preview is not None else asyncio.create_task(observe_preview(control, stopped, evidence))
            try:
                printed = await asyncio.to_thread(handle_function_call, 'kubik_printer', args,
                                                   tool_call_id='physical-image-once', enabled_tools=['kubik_printer'])
            finally:
                stopped.set()
                if monitor:
                    await monitor
            printed = json.loads(printed)
            if not document:
                assert peer.printer_preview_version == 1
                assert evidence['preview_active'] and evidence['preview_presented'], evidence
            assert printed.get('status') == 'sent', printed
            assert printed['layout']['format'] == ('document' if document else 'image') and printed['paperConfirmed'] is False
            assert observations.starts == 1 and observations.bands > 1
            return {'image_submitted': True, 'paper_confirmed': False, 'bands': observations.bands,
                    'layout': printed['layout'], 'source': 'synthetic local image', 'separate_turns': True, **evidence}
    finally:
        await adapter.on_processing_complete(event, 'success')
        reset_hermes_home_override(profile)
        clear_session_vars(tokens)
        path.unlink(missing_ok=True)


async def check_document(adapter, peer, control, providers, observations):
    return await check(adapter, peer, control, providers, observations, document=True)


class LiveObservations(Observations):
    """Record the physical preview window without widening the one-print budget."""
    def __init__(self, adapter):
        super().__init__(adapter)
        self.states, self.errors = [], []
        self.connection = None
        self.start_at = self.end_at = None
        self.print_rid = None
        self.authenticate = adapter.authenticate

    def install(self):
        import time
        super().install()
        parent = self.adapter.command
        async def command(peer, value):
            if (value.get('t') == 'printer_result' and value.get('rid') == self.print_rid
                    and value.get('status') in ('sent', 'error')):
                self.end_at = time.monotonic()
            return await parent(peer, value)
        self.adapter.command = command
        async def authenticate(socket):
            peer = await self.authenticate(socket)
            self.watch_socket(socket)
            return peer
        self.adapter.authenticate = authenticate

    def restore(self):
        super().restore()
        self.adapter.authenticate = self.authenticate

    def watch_socket(self, socket):
        import time
        if any(item[0] is socket for item in self.senders):
            return
        super().watch_socket(socket)
        guarded = socket.send_json
        async def send(value, **kwargs):
            if value.get('t') == 'printer_image' and value.get('action') == 'start':
                self.print_rid = value['rid']
                self.start_at = time.monotonic()
            return await guarded(value, **kwargs)
        socket.send_json = send

    def persist(self, evidence, providers):
        super().persist(evidence, providers)
        save(evidence / 'image-live-states.json', self.states)
        save(evidence / 'image-live-errors.json', self.errors)
        save(evidence / 'image-live-mic-times.json', self.connection.mic_times if self.connection else [])


async def monitor_live_image(control, observations, stopped, preview):
    import time
    fields = ('heap_failures', 'heap_internal', 'heap_dma', 'printer_busy', 'printer_preview_active',
              'printer_preview_presented', 'live_active', 'mic_open', 'mic_rx_enabled', 'online')
    try:
        while not stopped.is_set():
            state = await control.info()
            assert all(field in state for field in fields), 'Live image diagnostics incomplete'
            if len(observations.states) >= 2048:
                raise AssertionError('Live image observation limit')
            observations.states.append({'at': time.monotonic(), **{key: state[key] for key in fields}})
            preview['preview_active'] |= state['printer_preview_active']
            preview['preview_presented'] |= state['printer_preview_presented']
            await asyncio.sleep(.3)
    except Exception as error:
        observations.errors.append({'type': type(error).__name__, 'message': str(error)[:200]})


def shown_during_print(row, start, end):
    return (start <= row['at'] <= end and row['printer_busy'] and row['printer_preview_active']
            and row['printer_preview_presented'])


def live_input_ready(row):
    return row['live_active'] and row['mic_open'] and row['mic_rx_enabled'] and row['online']


def verify_live_image(observations, baseline):
    assert not observations.errors, observations.errors
    assert observations.start_at is not None and observations.end_at is not None
    rows = observations.states
    assert rows and all(row['heap_failures'] == baseline['heap_failures'] for row in rows), 'Heap allocation failed'
    active = [row for row in rows if shown_during_print(row, observations.start_at, observations.end_at)]
    assert len(active) >= 2, 'No sustained printed preview was presented during BLE work'
    assert all(live_input_ready(row) for row in active), 'Live microphone or connection stopped during preview'
    frames = [stamp for stamp in observations.connection.mic_times if active[0]['at'] < stamp < active[-1]['at']]
    assert frames, 'No Live microphone frame arrived during the observed preview window'
    return {'mic_frames_during_preview': len(frames), 'preview_samples': len(active),
            'heap_internal_min': min(row['heap_internal'] for row in rows),
            'heap_dma_min': min(row['heap_dma'] for row in rows)}


async def check_live_image(adapter, peer, control, providers, observations):
    from device_control import fault_window
    from device_printer import MicConnection, wait_discovery
    from device_voice import begin
    from voice_fixture import VoiceFixture
    observations.evidence = control.evidence
    assert peer.printer_preview_version == 1
    before = await control.request(cmd='bridge-diagnostics')
    await wait_discovery(control)
    baseline = await control.info()
    providers.voice = VoiceFixture(MicConnection)
    old_volume, stopped, monitor = peer.volume, asyncio.Event(), None
    peer.volume = 0
    try:
        observations.connection = await begin('live', adapter, peer, control, providers.voice)
        await control.sim('ptt_up')
        preview = {'preview_active': False, 'preview_presented': False}
        monitor = asyncio.create_task(monitor_live_image(control, observations, stopped, preview))
        result = await check(adapter, peer, control, providers, observations, external_preview=preview)
        await control.until(lambda state: not state['printer_busy'] and not state['printer_preview_active']
                            and not state['printer_preview_presented'], 'preview and printer released', timeout=15)
        stopped.set()
        await monitor
        monitor = None
        assert peer.native and peer.native.current(), 'Live closed during image printing'
        measured = verify_live_image(observations, baseline)
        fault_window(before, await control.request(cmd='bridge-diagnostics'))
        return {**result, 'live_microphone_during_preview': True, 'paid_provider_calls': 0, **measured}
    finally:
        stopped.set()
        if monitor:
            await monitor
        peer.volume = old_volume
        observations.persist(control.evidence, providers)
