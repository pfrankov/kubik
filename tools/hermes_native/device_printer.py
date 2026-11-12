"""Physical printer acceptance: real agent tools and board replies, never synthetic results."""
import asyncio
import base64
import importlib
import time
import uuid

from device_control import fault_window, save
from printer_contract import QUALITY_QR, QUALITY_TEXT, printer_arguments
from voice_fixture import VoiceConnection, VoiceFixture, eventually

MARKERS = ('kubik-printer-status', 'kubik-native-printer-text', 'kubik-native-printer-qr')
PLAN = [printer_arguments(marker)[1] for marker in MARKERS]
STREAM_PLAN = printer_arguments('kubik-printer-stream-check-' + '0' * 16)[1]
QUALITY_PLAN = [QUALITY_TEXT, QUALITY_QR]
QUALITY_EMOJI = ['😀', '❤️', '👍🏽', '👩‍💻', '🇷🇺']


def valid_glyphs(value, sequences):
    if not isinstance(value, list) or len(value) != len(sequences):
        return False
    try:
        for glyph, sequence in zip(value, sequences):
            if not isinstance(glyph, dict) or glyph.keys() != {'sequence', 'bitmap'} or glyph['sequence'] != sequence:
                return False
            bitmap = glyph['bitmap']
            if not isinstance(bitmap, str) or len(bitmap) != 172:
                return False
            decoded = base64.b64decode(bitmap, validate=True)
            if len(decoded) != 128 or base64.b64encode(decoded).decode() != bitmap:
                return False
    except ValueError:
        return False
    return True


class Observations:
    def __init__(self, adapter, *, plan=None, emoji_plan=None):
        self.adapter, self.wire, self.calls, self.cache = adapter, [], [], []
        self.command, self.authenticate = adapter.command, adapter.authenticate
        self.request = adapter.printer_jobs.request
        self.senders, self.evidence = [], None
        self.plan = PLAN if plan is None else plan
        self.emoji_plan = [[] for _ in self.plan] if emoji_plan is None else emoji_plan

    def install(self):
        async def authenticate(socket):
            peer = await self.authenticate(socket)
            self.watch_socket(socket)
            return peer
        async def command(peer, value):
            if value.get('t') == 'printer_result':
                self.record('device', value)
            return await self.command(peer, value)
        async def request(peer, call_id, arguments, event_id):
            if len(self.calls) >= len(self.plan):
                raise AssertionError('Physical acceptance model printer call budget exceeded')
            row = {'identity': peer.identity, 'session': peer.session_id, 'call_id': call_id,
                   'event_id': event_id, 'arguments': arguments}
            self.calls.append(row)
            self.persist_calls()
            row['result'] = await self.request(peer, call_id, arguments, event_id)
            self.persist_calls()
            return row['result']
        self.adapter.authenticate, self.adapter.command = authenticate, command
        self.adapter.printer_jobs.request = request

    def record(self, direction, value):
        if len(self.wire) >= 64:
            raise AssertionError('Physical printer observation limit')
        self.wire.append({'direction': direction, 'at': time.monotonic(), 'value': dict(value)})
        if self.evidence:
            save(self.evidence / 'printer-wire.json', self.wire)

    def watch_socket(self, socket):
        if any(existing is socket for existing, _ in self.senders):
            return
        original = socket.send_json
        self.senders.append((socket, original))
        async def send(value, **kwargs):
            if value.get('t') == 'printer':
                sent = self.requests()
                candidate = {key: item for key, item in value.items() if key not in ('t', 'rid')}
                expected = dict(self.plan[len(sent)]) if len(sent) < len(self.plan) else None
                if expected and expected['action'] == 'print':
                    expected.setdefault('format', 'text')
                glyphs = candidate.pop('emoji', None) if expected and expected.get('format') == 'text' else None
                if candidate != expected:
                    raise AssertionError('Unexpected or repeated physical printer request; refusing another print')
                if expected.get('format') == 'text' and not valid_glyphs(glyphs, self.emoji_plan[len(sent)]):
                    raise AssertionError('Unexpected internal emoji glyph payload')
                self.record('host', value)  # Persist before dispatch: an interrupted send is never replayed.
            return await original(value, **kwargs)
        socket.send_json = send

    def requests(self):
        return [row['value'] for row in self.wire if row['direction'] == 'host']

    def persist_calls(self):
        if self.evidence:
            save(self.evidence / 'printer-tools.json', self.calls)

    def persist(self, evidence, providers):
        self.evidence = evidence
        self.persist_calls()
        save(evidence / 'printer-wire.json', self.wire)
        save(evidence / 'printer-cache.json', self.cache)
        save(evidence / 'printer-model-calls.json', [body for kind, body in providers.calls if kind == 'llm'])

    def restore(self):
        self.adapter.command, self.adapter.authenticate = self.command, self.authenticate
        self.adapter.printer_jobs.request = self.request
        for socket, original in self.senders:
            socket.send_json = original


def verify_result(value, expected):
    assert value['status'] == ('ready' if expected['action'] == 'status' else 'sent'), value
    assert value['paperConfirmed'] is False, value
    printer = value['printer']
    assert printer['dpi'] == 304 and printer['width_dots'] == 576, printer
    assert printer['feed_dpi'] == 225 and printer['charging'] is None, printer
    assert printer['size_source'] == 'model_profile' and printer['media_height_mm'] is None, printer
    if expected['action'] == 'print':
        layout = value['layout']
        assert value['outcome'] == 'submitted' and layout['format'] == expected.get('format', 'text'), value
        assert abs(layout['width_mm'] - expected['width_mm']) <= 25.4 / printer['dpi'], value
        for field in ('height_mm', 'font_mm'):
            if field in expected:
                assert abs(layout[field] - expected[field]) <= 25.4 / printer['feed_dpi'], value


async def agent_turn(adapter, peer, marker):
    event = adapter._authorized_voice_event(peer, marker)
    assert event is not None and adapter._ready_peer(peer.identity) is peer
    async with asyncio.timeout(130):
        await adapter._run_voice_event(peer, event)
    await peer.send('state', s='idle')
    return event


async def cached_call(peer, observations, row):
    count = len(observations.requests())
    cached = await observations.request(peer, row['call_id'], row['arguments'], row['event_id'])
    assert cached == row['result'] and len(observations.requests()) == count
    observations.cache.append({'rid': cached['rid'], 'status': cached['status'], 'extra_device_requests': 0})


async def wait_discovery(control):
    stable, states = None, []
    def settled(state):
        nonlocal stable
        assert type(state.get('printer_busy')) is bool and type(state.get('printer_discovered')) is bool, 'Printer diagnostics missing'
        now = time.monotonic()
        if len(states) < 128:
            states.append({'at': now, 'busy': state['printer_busy'], 'discovered': state['printer_discovered']})
        if state['printer_busy'] or not state['printer_discovered']:
            stable = None
            return False
        stable = now if stable is None else stable
        return now - stable >= .5
    try:
        await control.until(settled, 'automatic printer discovery quiescent for 500 ms', timeout=40)
    finally:
        save(control.evidence / 'printer-discovery.json', states)


async def check(adapter, peer, control, providers, observations):
    observations.evidence = control.evidence
    package = type(adapter).__module__.rsplit('.', 1)[0]
    contract = importlib.import_module(package + '.printer_contract')
    assert peer.printer_version == contract.VERSION, 'Physical Cube requires printer protocol v2'
    before = await control.request(cmd='bridge-diagnostics')
    await wait_discovery(control)
    previous_volume, previous_stage = peer.volume, peer.stage
    peer.volume = 0  # Host peer only: preserve the device's saved speech/interface volumes.
    try:
        for index, marker in enumerate(MARKERS):
            event = await agent_turn(adapter, peer, marker)
            assert len(observations.calls) == index + 1, observations.calls
            row = observations.calls[-1]
            assert row['event_id'] == event.message_id and contract.parameters(row['arguments']) == contract.parameters(PLAN[index])
            verify_result(row['result'], PLAN[index])
            if index:
                await cached_call(peer, observations, row)
            observations.persist(control.evidence, providers)
        requests = observations.requests()
        assert len(requests) == 3 and sum(row['action'] == 'print' for row in requests) == 2, requests
        assert len({row['rid'] for row in requests}) == 3
        fault_window(before, await control.request(cmd='bridge-diagnostics'))
        return {'printer_tool': True, 'test_prints_submitted': 2, 'paper_confirmed': False,
                'text_width_mm': 20, 'qr_width_mm': 25, 'cache_no_repeat': True,
                'printer': observations.calls[0]['result']['printer'], 'paid_provider_calls': 0}
    finally:
        peer.volume, peer.stage = previous_volume, previous_stage


class MicConnection(VoiceConnection):
    def __init__(self, *args):
        super().__init__(*args)
        self.mic_times = []

    async def respond(self, event):
        if event['type'] == 'session.input_audio.append':
            if len(self.mic_times) >= 4000:
                raise AssertionError('Physical Live microphone observation limit')
            self.mic_times.append(time.monotonic())
        await super().respond(event)


class LiveObservations(Observations):
    def __init__(self, adapter):
        super().__init__(adapter, plan=[{'action': 'status'}])
        self.states, self.errors, self.voice_wire = [], [], []
        self.connection = None
        self.phase = 'starting'

    def record(self, direction, value):
        super().record(direction, value)
        self.persist_live()

    def watch_socket(self, socket):
        if any(existing is socket for existing, _ in self.senders):
            return
        super().watch_socket(socket)
        original = socket.send_json
        async def send(value, **kwargs):
            if value.get('t') in ('speak', 'speak_end', 'speak_cancel', 'live_input', 'error'):
                if len(self.voice_wire) >= 64:
                    raise AssertionError('Physical Live control observation limit')
                self.voice_wire.append({'at': time.monotonic(), **value})
                self.persist_live()
            return await original(value, **kwargs)
        socket.send_json = send

    def persist_live(self):
        if self.evidence:
            save(self.evidence / 'printer-live-states.json', self.states)
            save(self.evidence / 'printer-live-input.json', self.connection.mic_times if self.connection else [])
            save(self.evidence / 'printer-live-control.json', self.voice_wire)
            save(self.evidence / 'printer-live-errors.json', self.errors)

    def persist(self, evidence, providers):
        super().persist(evidence, providers)
        self.persist_live()


async def sample_live(control, observations, phase):
    state = await control.info()
    fields = ('heap', 'heap_internal', 'heap_dma', 'heap_min', 'heap_failures', 'mic_reads',
              'mic_open', 'mic_rx_enabled', 'live_active', 'live_ready', 'printer_busy',
              'printer_discovered', 'online', 'via')
    assert all(field in state for field in fields), 'Physical Live diagnostics incomplete'
    row = {'at': time.monotonic(), 'phase': phase, **{field: state[field] for field in fields}}
    observations.states.append(row)
    observations.persist_live()
    return row


async def monitor_live(control, observations, stopped):
    try:
        while not stopped.is_set():
            if len(observations.states) >= 256:
                raise AssertionError('Physical Live status observation limit')
            await sample_live(control, observations, observations.phase)
            await asyncio.sleep(.25)
    except Exception as error:
        observations.errors.append({'type': type(error).__name__, 'message': str(error)[:200]})
        observations.persist_live()


def verify_live_states(observations, baseline):
    assert not observations.errors, observations.errors
    for row in observations.states:
        assert row['heap_failures'] == baseline['heap_failures'], row
        if row['phase'] == 'live':
            assert row['live_active'] and row['mic_open'] and row['mic_rx_enabled'] and row['online'], row
    assert not [row for row in observations.voice_wire if row['t'] == 'speak'], 'Unexpected output in status-only Live test'
    return {'heap_internal_min': min(row['heap_internal'] for row in observations.states),
            'heap_dma_min': min(row['heap_dma'] for row in observations.states)}


def verify_live_window(observations, baseline):
    requests = observations.requests()
    assert len(requests) == 1 and requests[0]['action'] == 'status', requests
    rows = observations.wire
    start = next(row['at'] for row in rows if row['direction'] == 'host')
    end = next(row['at'] for row in rows if row['direction'] == 'device' and row['value'].get('rid') == requests[0]['rid'])
    during = [stamp for stamp in observations.connection.mic_times if start < stamp < end]
    assert during, 'No provider microphone frame arrived during the actual printer RPC'
    return {'mic_frames_during_rpc': len(during), 'rpc_ms': (end - start) * 1000,
            **verify_live_states(observations, baseline)}


async def check_live_status(adapter, peer, control, providers, observations):
    from device_voice import begin
    observations.evidence = control.evidence
    assert peer.printer_version == 2, 'Physical Cube requires printer protocol v2'
    before = await control.request(cmd='bridge-diagnostics')
    await wait_discovery(control)
    baseline = await sample_live(control, observations, 'baseline')
    providers.voice = VoiceFixture(MicConnection)
    old_volume, stopped, monitor = peer.volume, asyncio.Event(), None
    peer.volume = 0
    token = uuid.uuid4().hex[:16]
    try:
        monitor = asyncio.create_task(monitor_live(control, observations, stopped))
        connection = observations.connection = await begin('live', adapter, peer, control, providers.voice)
        observations.phase = 'live'
        await control.sim('ptt_up')
        await sample_live(control, observations, 'live')
        await connection.delegate('kubik-native-printer-live-status-' + token, 'printer-live-status-' + token, 100)
        await eventually(lambda: len(observations.calls) == 1 and 'result' in observations.calls[0], timeout=30)
        row = observations.calls[0]
        assert row['call_id'] == 'call-printer-live-status-' + token and row['arguments'] == {'action': 'status'}, row
        verify_result(row['result'], {'action': 'status'})
        await eventually(lambda: any('kubik-printer-answer:live-status-' + token in str(item)
                                     for item in connection.events('session.commentary.append')))
        await sample_live(control, observations, 'live')
        stopped.set()
        await monitor
        monitor = None
        assert peer.native and peer.native.current(), 'Live closed during status-only printer work'
        measured = verify_live_window(observations, baseline)
        fault_window(before, await control.request(cmd='bridge-diagnostics'))
        return {'printer_live_status': True, 'printer_tool': True, 'test_prints_submitted': 0,
                'continuous_microphone': True, 'printer': row['result']['printer'],
                'paid_provider_calls': 0, **measured}
    finally:
        stopped.set()
        if monitor:
            await monitor
        if observations.connection is None and providers.voice.connections:
            observations.connection = providers.voice.connections[-1]
        peer.volume = old_volume
        observations.persist(control.evidence, providers)


class StreamObservations(Observations):
    def __init__(self, adapter):
        super().__init__(adapter, plan=[STREAM_PLAN])
        self.states = []
        self.state_file = 'printer-stream-states.json'

    def persist(self, evidence, providers):
        super().persist(evidence, providers)
        save(evidence / self.state_file, self.states)


async def stream_state(control, observations, phase):
    state = await control.info()
    fields = ('heap', 'heap_internal', 'heap_dma', 'heap_min', 'heap_failures', 'printer_busy', 'printer_discovered')
    assert all(field in state for field in fields), 'Physical stream diagnostics incomplete'
    row = {'at': time.monotonic(), 'phase': phase, **{field: state[field] for field in fields}}
    observations.states.append(row)
    save(control.evidence / observations.state_file, observations.states)
    return row


async def check_stream(adapter, peer, control, providers, observations):
    observations.evidence = control.evidence
    assert peer.printer_version == 2, 'Physical Cube requires printer protocol v2'
    before = await control.request(cmd='bridge-diagnostics')
    await wait_discovery(control)
    baseline = await stream_state(control, observations, 'before')
    old_volume, old_stage = peer.volume, peer.stage
    peer.volume = 0
    token = uuid.uuid4().hex[:16]
    try:
        event = await agent_turn(adapter, peer, 'kubik-printer-stream-check-' + token)
        assert len(observations.calls) == 1, observations.calls
        row = observations.calls[0]
        assert row['call_id'] == 'call-printer-stream-check-' + token and row['event_id'] == event.message_id, row
        verify_result(row['result'], STREAM_PLAN)
        assert len(observations.requests()) == 1 and observations.requests()[0]['action'] == 'print'
        after = await stream_state(control, observations, 'after')
        assert after['heap_failures'] == baseline['heap_failures'], after
        fault_window(before, await control.request(cmd='bridge-diagnostics'))
        return {'printer_stream_check': True, 'printer_tool': True, 'test_prints_submitted': 1,
                'paper_confirmed': False, 'layout': row['result']['layout'],
                'printer': row['result']['printer'], 'paid_provider_calls': 0}
    finally:
        peer.volume, peer.stage = old_volume, old_stage
        observations.persist(control.evidence, providers)


class QualityObservations(StreamObservations):
    def __init__(self, adapter):
        Observations.__init__(self, adapter, plan=QUALITY_PLAN, emoji_plan=[QUALITY_EMOJI, []])
        self.states, self.state_file = [], 'printer-quality-states.json'


async def check_quality(adapter, peer, control, providers, observations):
    observations.evidence = control.evidence
    assert peer.printer_version == 2, 'Physical Cube requires printer protocol v2'
    before = await control.request(cmd='bridge-diagnostics')
    await wait_discovery(control)
    baseline = await stream_state(control, observations, 'before')
    old_volume, old_stage = peer.volume, peer.stage
    peer.volume = 0
    token = uuid.uuid4().hex[:16]
    try:
        for index, kind in enumerate(('text', 'qr')):
            event = await agent_turn(adapter, peer, 'kubik-printer-quality-' + kind + '-' + token)
            assert len(observations.calls) == index + 1, observations.calls
            row = observations.calls[index]
            assert row['call_id'] == 'call-printer-quality-' + kind + '-' + token and row['event_id'] == event.message_id
            verify_result(row['result'], QUALITY_PLAN[index])
            await stream_state(control, observations, kind + '-submitted')
        assert len(observations.requests()) == 2 and all(row['action'] == 'print' for row in observations.requests())
        qr = observations.calls[1]['result']['layout']
        printer = observations.calls[1]['result']['printer']
        assert abs(qr['width_mm'] - qr['height_mm']) <= 25.4 / printer['dpi'] + 25.4 / printer['feed_dpi'], qr
        assert all(row['heap_failures'] == baseline['heap_failures'] for row in observations.states)
        fault_window(before, await control.request(cmd='bridge-diagnostics'))
        return {'printer_quality_check': True, 'test_prints_submitted': 2, 'paper_confirmed': False,
                'apple_emoji_used': QUALITY_EMOJI, 'layouts': [row['result']['layout'] for row in observations.calls],
                'printer': observations.calls[0]['result']['printer'], 'paid_provider_calls': 0}
    finally:
        peer.volume, peer.stage = old_volume, old_stage
        observations.persist(control.evidence, providers)
