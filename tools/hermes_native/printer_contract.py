"""Actual Hermes model/tool dispatch to an authenticated printer-capable device."""
import asyncio
import base64
import importlib
import json
from pathlib import Path
import re

from cryptography.hazmat.primitives.asymmetric import ec
from voice_contract import Device
from voice_fixture import eventually

PROFILE = {'name': 'PeriPage+FIXTURE', 'model': 'PeriPage A6+', 'firmware': 'V1.12_304dpi',
           'hardware': 'v3.38.21_AY', 'dpi': 304, 'feed_dpi': 225, 'width_dots': 576,
           'printable_width_mm': 576 * 25.4 / 304, 'battery_pct': 100, 'charging': None,
           'formats': ['text', 'qr'], 'max_height_mm': 150, 'media_height_mm': None,
           'size_source': 'model_profile'}
QUALITY_TEXT = {'action': 'print', 'text': 'Apple 😀 ❤️ 👍🏽 👩‍💻 🇷🇺', 'width_mm': 40, 'font_mm': 3}
QUALITY_QR = {'action': 'print', 'text': 'KUBIK 304x225', 'format': 'qr', 'width_mm': 20}


def printer_response(request):
    value = {'t': 'printer_result', 'rid': request['rid'], 'status': 'ready', 'printer': PROFILE}
    if request['action'] == 'print':
        width = round(request.get('width_mm', PROFILE['printable_width_mm']) * 304 / 25.4)
        extent = request.get('width_mm', PROFILE['printable_width_mm']) if request.get('format') == 'qr' else 10
        height = round(request.get('height_mm', extent) * 225 / 25.4)
        value.update(status='sent', outcome='submitted', layout={'format': request.get('format', 'text'),
                     'width_dots': width, 'height_dots': height,
                     'width_mm': width * 25.4 / 304, 'height_mm': height * 25.4 / 225})
        if 'font_mm' in request:
            value['layout']['font_mm'] = request['font_mm']
    return value


def printer_arguments(user):
    if 'kubik-printer-status' in user:
        return 'status', {'action': 'status'}
    live = re.search(r'kubik-native-printer-live-status-([0-9a-f]{16})', user)
    if live:
        return 'live-status-' + live[1], {'action': 'status'}
    stream = re.search(r'kubik-printer-stream-check-([0-9a-f]{16})', user)
    if stream:
        return 'stream-check-' + stream[1], {'action': 'print', 'text': 'BLE 20 ms',
                    'width_mm': 30, 'height_mm': 10, 'font_mm': 3}
    quality = re.search(r'kubik-printer-quality-(text|qr)-([0-9a-f]{16})', user)
    if quality:
        return 'quality-' + quality[1] + '-' + quality[2], dict(QUALITY_TEXT if quality[1] == 'text' else QUALITY_QR)
    elif 'kubik-native-printer-text' in user:
        return 'text', {'action': 'print', 'text': 'Список\nМолоко', 'width_mm': 20}
    elif 'kubik-native-printer-qr' in user:
        return 'qr', {'action': 'print', 'text': 'https://example.com', 'format': 'qr', 'width_mm': 25}
    return None


def printer_answer(messages):
    index = next(i for i in range(len(messages) - 1, -1, -1) if messages[i]['role'] == 'user')
    request = printer_arguments(str(messages[index]['content']))
    if request is None:
        return None
    marker, arguments = request
    status = arguments['action'] == 'status'
    tools = [json.loads(item['content']) for item in messages[index + 1:] if item['role'] == 'tool']
    if tools:
        value = tools[-1]
        assert 'printer' in value, value
        assert value['printer']['width_dots'] == 576 and value['printer']['dpi'] == 304, value
        assert value['printer']['feed_dpi'] == 225 and value['printer']['charging'] is None, value
        assert value['paperConfirmed'] is False and value['status'] == ('ready' if status else 'sent'), value
        if not status:
            assert value['layout']['format'] == arguments.get('format', 'text'), value
            assert value['outcome'] == 'submitted', value
        return {'role': 'assistant', 'content': 'kubik-printer-answer:' + marker}, 'stop'
    call = {'id': 'call-printer-' + marker, 'type': 'function',
            'function': {'name': 'kubik_printer', 'arguments': json.dumps(arguments)}}
    return {'role': 'assistant', 'content': None, 'tool_calls': [call]}, 'tool_calls'


class PrinterDevice(Device):
    def __init__(self, socket, codec):
        self.requests = []
        self.hold_printer = False
        self.image = None
        self.image_done = False
        self.all_image_jobs = []
        self.image_bands = []
        self.hold_image_after = None
        self.image_aborts = 0
        self.hold_abort_reply = False
        super().__init__(socket, codec)

    async def binary(self, data):
        if data[0] != 6:
            return await super().binary(data)
        image = self.image
        assert image and data[1] == 0 and data[2:34].decode() == image['rid']
        offset, count = int.from_bytes(data[34:36], 'big'), data[36]
        assert offset == sum(self.image_bands) and 1 <= count <= 16 and len(data) == 37 + count * 72
        self.image_bands.append(count)
        if offset + count < image['height']:
            if self.hold_image_after is None or len(self.image_bands) < self.hold_image_after:
                await self.send(t='printer_image_ready', rid=image['rid'], offset=offset + count)
        else:
            layout = {'format': 'image', 'width_dots': image['width'], 'height_dots': image['height'],
                      'width_mm': image['width'] * 25.4 / 304, 'height_mm': image['height'] * 25.4 / 225}
            self.image_done = True
            await self.send(t='printer_result', rid=image['rid'], status='sent', outcome='submitted',
                            printer=PROFILE, layout=layout)

    async def acknowledge(self, value):
        if value['t'] == 'printer_image':
            if value['action'] == 'abort':
                self.image_aborts += 1
                if not self.hold_abort_reply:
                    await self.send(t='printer_result', rid=value['rid'], status='error',
                                    code='printer_disconnected', outcome='unknown')
            else:
                assert value['action'] == 'start' and (self.image is None or self.image_done)
                self.image = value
                self.image_done = False
                self.image_bands = []
                self.all_image_jobs.append(value['rid'])
                await self.send(t='printer_image_ready', rid=value['rid'], offset=0)
        elif value['t'] == 'printer':
            if value['action'] == 'print' and value.get('format') == 'text':
                expected = ['😀', '❤️', '👍🏽', '👩‍💻', '🇷🇺'] if value['text'] == QUALITY_TEXT['text'] else []
                glyphs = value.get('emoji')
                assert isinstance(glyphs, list) and [glyph['sequence'] for glyph in glyphs] == expected, value
                assert all(len(base64.b64decode(glyph['bitmap'], validate=True)) == 128 for glyph in glyphs), value
            else:
                assert 'emoji' not in value, value
            self.requests.append(value)
            if not self.hold_printer:
                await self.socket.send_json(printer_response(value))
        else:
            await super().acknowledge(value)


async def classic(device, providers):
    providers.transcript = 'kubik-printer-status'
    start = len(device.events)
    await device.send(t='ptt', on=True, turn=61)
    for _ in range(3):
        await device.microphone(61)
    await device.send(t='ptt', on=False, turn=61, ms=120)
    await device.wait(lambda item: item['t'] == 'text' and 'kubik-printer-answer:status' in item['text'], start)
    await device.wait(lambda item: item.get('s') == 'idle', start)
    assert [item['action'] for item in device.requests] == ['status'], device.requests


async def realtime(device, providers):
    await device.select('mode', 'realtime')
    start, index = len(device.events), len(providers.voice.connections)
    await device.send(t='ptt', on=True, turn=62)
    connection = await providers.voice.opened(index)
    await device.wait(lambda item: item.get('s') == 'listening', start)
    for _ in range(3):
        await device.microphone(62)
    await eventually(lambda: len(connection.events('input_audio_buffer.append')) == 3)
    await connection.finish_input('kubik-native-printer-text')
    await device.wait(lambda item: item['t'] == 'text' and 'kubik-printer-answer:text' in item['text'], start)
    await device.wait(lambda item: item.get('s') == 'idle', start)
    assert device.requests[-1]['action'] == 'print' and device.requests[-1]['width_mm'] == 20


async def live(device, providers):
    await device.select('mode', 'live')
    await device.send(t='device_state', volume=100)
    start, index = len(device.events), len(providers.voice.connections)
    await device.send(t='ptt', on=True, turn=63)
    connection = await providers.voice.opened(index)
    await device.wait(lambda item: item['t'] == 'live_input' and item['on'], start)
    await device.microphone(63, live=True)
    await connection.audio()
    await device.wait(lambda item: item['t'] == 'audio', start)
    device.hold_printer = True
    await connection.delegate('kubik-native-printer-qr', 'printer-qr', 200)
    await device.wait(lambda item: item['t'] == 'printer', start)
    held = len(device.events)
    await connection.audio(bytes(7200))
    await connection.audio()
    before_input = len(connection.events('session.input_audio.append'))
    await device.microphone(63, live=True)
    await eventually(lambda: len(connection.events('session.input_audio.append')) > before_input)
    await asyncio.sleep(.05)
    assert not [item for item in device.events[held:] if item['t'] in ('speak', 'audio')], device.events[held:]
    await device.socket.send_json(printer_response(device.requests[-1]))
    device.hold_printer = False
    await device.wait(lambda item: item['t'] == 'audio', held)
    await eventually(lambda: any('kubik-printer-answer:qr' in json.dumps(item) for item in connection.events('session.commentary.append')))
    assert device.requests[-1]['format'] == 'qr' and device.requests[-1]['width_mm'] == 25
    await device.send(t='cancel', turn=63)
    await device.wait(lambda item: item.get('s') == 'idle', start)


async def check_printer_tools(adapter, providers, client, port, home, codec, pair):
    key = ec.generate_private_key(ec.SECP256R1())
    device = PrinterDevice(await pair(client, port, home, key, name='kubik-printer', volume=0), codec)
    original = providers.transcript
    try:
        await classic(device, providers)
        await realtime(device, providers)
        await live(device, providers)
        assert len(device.requests) == 3 and len({row['rid'] for row in device.requests}) == 3
        from model_tools import handle_function_call
        result = await asyncio.to_thread(handle_function_call, 'kubik_printer', {'action': 'print', 'text': 'X'},
                                         tool_call_id='outside-kubik', enabled_tools=['kubik_printer'])
        assert json.loads(result)['code'] == 'unauthorized', result
        assert len(device.requests) == 3
        await synthetic_emoji_transport(adapter, device, home)
        from image_contract import check_image_hook
        await check_image_hook(adapter, device, home)
        await reject_foreign_context(adapter, client, port, home, codec, pair, key)
        print('PASS native Hermes printer tool in Classic/Realtime/Live, actual call IDs, audio lease, continuous mic and outside-channel rejection', flush=True)
    finally:
        providers.transcript = original
        await device.close()


async def synthetic_emoji_transport(adapter, device, home):
    from device_control import save
    from device_printer import QUALITY_EMOJI, agent_turn
    path = Path(__file__).resolve().parents[1] / 'fixtures/hermes_emoji_support.py'
    specification = importlib.util.spec_from_file_location('synthetic_emoji_atlas', path)
    fixture = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(fixture)
    state = home / 'kubik/apple-emoji.bin'
    save(state, fixture.atlas(QUALITY_EMOJI))
    peer = next(peer for peer in adapter.peers.values() if peer.identity.startswith('kubik-printer:'))
    old_volume = peer.volume
    peer.volume = 0
    try:
        event = await agent_turn(adapter, peer, 'kubik-printer-quality-text-0123456789abcdef')
        assert event._gateway_accepted
        request = device.requests[-1]
        assert request['text'] == QUALITY_TEXT['text'] and len(request['emoji']) == 5, request
        assert len(device.requests) == 4
        print('PASS actual Hermes tool dispatch with private synthetic KEMJ state and only five used glyphs', flush=True)
    finally:
        peer.volume = old_volume
        state.unlink()


async def reject_foreign_context(adapter, client, port, home, codec, pair, key):
    from gateway.session_context import set_session_vars, clear_session_vars
    from hermes_constants import set_hermes_home_override, reset_hermes_home_override
    from model_tools import handle_function_call
    printer_origin = importlib.import_module(type(adapter).__module__.rsplit('.', 1)[0] + '.printer_tool').printer_origin
    peer = next(peer for peer in adapter.peers.values() if peer.identity.startswith('kubik-printer:'))
    event = adapter._authorized_voice_event(peer, 'printer-security-probe')
    async def invoke():
        result = await asyncio.to_thread(handle_function_call, 'kubik_printer', {'action': 'print', 'text': 'X'},
                                         tool_call_id='context-security', enabled_tools=['kubik_printer'])
        return json.loads(result)
    def session(platform='kubik'):
        return set_session_vars(platform=platform, chat_id=peer.identity, message_id=event.message_id,
                                session_key=adapter._event_session_key(event))
    replacement = None
    with printer_origin(adapter, peer, event):
        tokens = session('telegram')
        try:
            assert (await invoke())['code'] == 'unauthorized'
        finally:
            clear_session_vars(tokens)
        tokens, profile = session(), set_hermes_home_override(home / 'other-profile')
        try:
            foreign = await invoke()
            assert foreign.get('error') == 'Unknown tool: kubik_printer', foreign
        finally:
            reset_hermes_home_override(profile)
            clear_session_vars(tokens)
        tokens = session()
        try:
            replacement = PrinterDevice(await pair(client, port, home, key, name='kubik-printer', volume=0, mode='live'), codec)
            assert (await invoke())['code'] == 'device_replaced'
            assert not replacement.requests
        finally:
            clear_session_vars(tokens)
            if replacement:
                await replacement.close()
