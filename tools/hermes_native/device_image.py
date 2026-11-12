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


async def check(adapter, peer, control, providers, observations):
    from gateway.session_context import set_session_vars, clear_session_vars
    from hermes_constants import set_hermes_home_override, reset_hermes_home_override
    from model_tools import handle_function_call
    from image_contract import transform
    from PIL import Image, ImageDraw
    observations.persist(control.evidence, providers)
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
            args = {'action': 'print', 'format': 'image', 'image_ref': result['kubik_image_ref'],
                    'width_mm': 20, 'height_mm': 20}
            printed = await asyncio.to_thread(handle_function_call, 'kubik_printer', args,
                                               tool_call_id='physical-image-once', enabled_tools=['kubik_printer'])
            printed = json.loads(printed)
            assert printed.get('status') == 'sent', printed
            assert printed['layout']['format'] == 'image' and printed['paperConfirmed'] is False
            assert observations.starts == 1 and observations.bands > 1
            return {'image_submitted': True, 'paper_confirmed': False, 'bands': observations.bands,
                    'layout': printed['layout'], 'source': 'synthetic local image'}
    finally:
        await adapter.on_processing_complete(event, 'success')
        reset_hermes_home_override(profile)
        clear_session_vars(tokens)
        path.unlink(missing_ok=True)
