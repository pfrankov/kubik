"""Genuine Gateway color glyph transport with signed synthetic device and synthetic KEMC."""
import asyncio
import base64
import importlib
import importlib.util
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric import ec
from device_control import save
from device_printer import agent_turn
from device_screen_emoji import MARKER, ONLY_MARKER, ONLY_TEXT, SEQUENCES, TEXT
from voice_contract import Device
from voice_fixture import eventually


class ScreenDevice(Device):
    def __init__(self, socket, codec, glyphs):
        self.glyphs, self.pending, self.pages, self.completed = glyphs, [], [], []
        self.identifier, self.page_requests = 1000, 0
        super().__init__(socket, codec)

    async def next_page(self):
        if not self.pages:
            return
        self.identifier += 1
        self.pending = self.pages.pop(0)
        self.page_requests += 1
        await self.send(t='emoji_get', id=self.identifier, seq=list(self.pending))

    async def acknowledge(self, value):
        await super().acknowledge(value)
        if value['t'] == 'text' and value['text'] in (TEXT, ONLY_TEXT):
            keys = SEQUENCES if value['text'] == TEXT else SEQUENCES[:5]
            self.pages = [keys[index:index + 4] for index in range(0, len(keys), 4)]
            self.completed = []
            await self.next_page()
        elif value['t'] == 'emoji':
            assert value['id'] == self.identifier and value['seq'] in self.pending, value
            glyph = base64.b64decode(value['data'], validate=True)
            assert len(glyph) == 576 and glyph == self.glyphs[value['seq']]
            self.pending.remove(value['seq'])
            self.completed.append(value['seq'])
            if not self.pending:
                await asyncio.sleep(.55)  # Respect the production two-page-per-second lookup budget.
                await self.next_page()


async def check_screen_transport(adapter, providers, client, port, home, codec, pair):
    path = Path(__file__).resolve().parents[1] / 'fixtures/hermes_color_emoji_support.py'
    specification = importlib.util.spec_from_file_location('synthetic_color_atlas', path)
    fixture = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(fixture)
    data, state = fixture.atlas(SEQUENCES), home / 'kubik/apple-emoji-color.bin'
    module = importlib.import_module(type(adapter).__module__.rsplit('.', 1)[0] + '.screen_emoji')
    save(state, data)
    device = ScreenDevice(await pair(client, port, home, ec.generate_private_key(ec.SECP256R1()),
                                       name='kubik-screen', volume=0), codec, module.parse_color(data))
    before = len(providers.calls)
    try:
        peer = next(peer for peer in adapter.peers.values() if peer.identity.startswith('kubik-screen:'))
        await agent_turn(adapter, peer, MARKER + '0123456789abcdef')
        await eventually(lambda: len(device.completed) == len(SEQUENCES))
        await agent_turn(adapter, peer, ONLY_MARKER + '0123456789abcdef')
        await eventually(lambda: len(device.completed) == 5 and not device.pages)
        assert device.page_requests == 5
        assert not [kind for kind, _body in providers.calls[before:] if kind in ('stt', 'tts')]
        print('PASS genuine Hermes agent color emoji cards, five signed page requests, emoji-only text, no STT/TTS/printing', flush=True)
    finally:
        await device.close()
        state.unlink()
