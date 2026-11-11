"""Bounded USB control and reversible runtime-only routing for physical voice acceptance."""
import asyncio
import base64
import hashlib
import json
import os
from pathlib import Path
import time
from urllib.parse import urlsplit

import aiohttp

PRESERVED = ('device', 'key', 'agent', 'ssid', 'url', 'character', 'server_pinned',
             'volume', 'ui_volume', 'brightness', 'guide_done')


def save(path, value):
    data = value if isinstance(value, bytes) else json.dumps(value, indent=2).encode()
    descriptor = os.open(path, os.O_CREAT | os.O_TRUNC | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, 'wb') as stream:
        stream.write(data)


def verify_settings(original, current):
    for field in PRESERVED:
        if field == 'ssid' and not original['online']:
            # wifi.c rotates the selected saved profile in RAM while offline; this is not an NVS update.
            if not isinstance(current[field], str) or len(current[field].encode()) > 32:
                raise AssertionError('Invalid current Wi-Fi candidate')
        elif current[field] != original[field]:
            raise AssertionError('Device setting changed: ' + field)


def screen_flags(state):
    values = state['screen_dark'], state['screen_sleeping']
    if not all(type(value) is bool for value in values):
        raise ValueError('Invalid screen power flags')
    return values


def screen_restore_action(original, current):
    target, actual = screen_flags(original), screen_flags(current)
    if target == actual:
        return 'done'
    if target[1]:
        if actual[1]:
            return 'wait' if target[0] else 'power'
        return 'power' if actual[0] else 'wait'
    return 'power'


class Control:
    def __init__(self, url, evidence):
        parsed = urlsplit(url)
        if parsed.scheme != 'http' or parsed.hostname != '127.0.0.1' or parsed.path not in ('', '/'):
            raise ValueError('USB control must be an explicit loopback HTTP URL')
        if parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise ValueError('USB control must not contain credentials, query or fragment')
        self.url, self.evidence = url.rstrip('/'), Path(evidence)
        self.lock, self.last = asyncio.Lock(), 0
        self.http = aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=5))

    async def request(self, **value):
        async with self.lock:
            await asyncio.sleep(max(0, self.last + .15 - time.monotonic()))
            self.last = time.monotonic()
            async with self.http.post(self.url + '/config', json=value) as response:
                if response.status != 200:
                    raise RuntimeError(f'USB {value["cmd"]} returned HTTP {response.status}')
                result = await response.json()
                if result.get('ok') is not True:
                    raise RuntimeError(f'USB {value["cmd"]} rejected')
                return result

    async def info(self):
        return await self.request(cmd='info')

    async def sim(self, event, **fields):
        return await self.request(cmd='sim', ev=event, **fields)

    async def until(self, predicate, label, timeout=30):
        try:
            async with asyncio.timeout(timeout):
                while True:
                    state = await self.info()
                    if predicate(state):
                        return state
                    await asyncio.sleep(.15)
        except TimeoutError:
            raise RuntimeError('Timed out waiting for ' + label) from None

    async def baseline(self):
        original = await self.info()
        for field in PRESERVED + ('voice_mode', 'online', 'via', 'wifi_connected', 'screen_dark', 'screen_sleeping', 'menu'):
            if field not in original:
                raise ValueError('Incomplete device status: ' + field)
        screen_flags(original)
        if original['via'] not in ('wifi', 'none'):
            raise ValueError('Release the previous USB fixture before this test')
        if any(original.get(field) is not False for field in ('mic_open', 'live_active', 'auto_recording')):
            raise ValueError('Physical device has an active conversation')
        raw = base64.b64decode(original['key'], validate=True)
        if len(raw) != 65 or original['voice_mode'] not in ('classic', 'realtime', 'live'):
            raise ValueError('Invalid physical identity or voice mode')
        save(self.evidence / 'before.json', original)
        return original, original['device'] + ':' + hashlib.sha256(raw).hexdigest()[:32]

    async def prepare_screen(self):
        state = await self.info()
        if state['screen_dark']:
            await self.sim('pwr')
        if state.get('setup_ap'):
            await self.sim('boot')
            await self.until(lambda item: not item.get('setup_ap'), 'setup closed')
        if (await self.info())['menu']:
            await self.sim('menu')

    async def restore(self, original, *, release_wifi_pause=True):
        if release_wifi_pause:
            await self.sim('wifi', ms=1)
        def restored(state):
            if original['online']:
                return (state['online'] and state['via'] == original['via'] and
                        state['wifi_connected'] == original['wifi_connected'])
            return not state['online'] and state['via'] == 'none' or (
                state['online'] and state['via'] == 'wifi' and state['wifi_connected'])
        state = await self.until(restored, 'original route restored', timeout=45)
        verify_settings(original, state)
        assert state['voice_mode'] == original['voice_mode'], 'Original voice mode was not restored'
        if state['menu'] != original['menu']:
            if state['screen_dark']:
                await self.sim('pwr')
                await self.until(lambda item: not item['screen_dark'], 'wake before restoring menu')
            await self.sim('menu')
            await self.until(lambda item: item['menu'] == original['menu'], 'original menu restored')
        state = await self.restore_screen(original)
        assert state['menu'] == original['menu'], 'Original menu changed while restoring screen'
        save(self.evidence / 'after.json', state)
        print('PASS physical device identity, settings, route and screen restored', flush=True)

    async def restore_screen(self, original):
        powers = 0
        async with asyncio.timeout(220 if original['screen_sleeping'] else 15):
            while True:
                state = await self.info()
                action = screen_restore_action(original, state)
                if action == 'done':
                    return state
                if action == 'power':
                    if powers >= 2:
                        raise RuntimeError('Screen power restoration exceeded two PWR transitions')
                    powers += 1
                    previous = screen_flags(state)
                    await self.sim('pwr')
                    await self.until(lambda item: screen_flags(item) != previous, 'screen power transition', timeout=5)
                else:
                    await asyncio.sleep(1)  # Natural 180 s sleep + 15 s fade; never turn it into manual-off.


def fault_window(before, after):
    assert before['instance'] == after['instance'], 'USB collector restarted'
    for key in ('faults', 'boots', 'disconnects'):
        assert type(after[key]) is int and after[key] == before[key], 'USB fault increased: ' + key


def playback_window(before, after, duration):
    assert after['sequence'] > before['sequence'] and after['active'] < 0
    assert after['reason'] == 'played' and after['ms'] == duration, after
    for key in ('gaps', 'gap_ms', 'rx_rejected', 'write_errors', 'late_feeds'):
        assert after[key] == 0, f'Speaker {key}: {after[key]}'
    assert after['heap_failures'] == before['heap_failures'], 'Speaker allocation failed'
