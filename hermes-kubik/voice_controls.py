"""Device voice selectors use the existing catalogue protocol and paired identity."""
import asyncio

from .voice_settings import (MODE_LABELS, MODES, VoicePreferences,
                             native_status)


class VoiceControls:
    def __init__(self, adapter):
        self.adapter = adapter
        self.store = VoicePreferences(adapter.security.home)
        self.operations = set()

    def choices(self, peer):
        return self.store.lookup(peer.identity)

    def mode(self, peer):
        return self.choices(peer).get('mode', 'classic')

    async def capabilities(self, peer):
        choices = self.choices(peer)
        mode = choices.get('mode', 'classic')
        if mode == 'classic':
            stt, tts = await asyncio.to_thread(self.adapter.speech_available)
        else:
            stt = await asyncio.to_thread(native_status, self.adapter.home, mode, choices.get(mode))
            tts = dict(stt)
        return {'stt': stt, 'tts': tts, 'voice_mode': mode}

    async def publish(self, peer):
        result = await self.capabilities(peer)
        if self.current(peer):
            await peer.send('capabilities', **result)

    def current(self, peer):
        return (self.adapter.peers.get(peer.identity) is peer and not peer.socket.closed and
                self.adapter._is_approved(peer.identity))

    async def catalogue(self, peer, target):
        selected = self.choices(peer)
        mode = selected.get('mode', 'classic')
        caps = await self.capabilities(peer)
        if target == 'mode':
            rows = []
            for key, label in zip(MODES, MODE_LABELS):
                status = caps['stt'] if key == mode else await self.mode_status(key)
                rows.append({'id': key, 'label': label, 'available': bool(status['available'])})
            return mode, rows, caps
        if target != 'voice' or mode == 'classic':
            raise ControlError('unsupported')
        status = await self.mode_status(mode)
        model = 'openai/' + status['model'] if status.get('model') else ''
        rows = [{'id': model, 'label': status['model'], 'available': True}] if model else []
        return selected.get(mode, model), rows, caps

    async def mode_status(self, mode):
        if mode == 'classic':
            return (await asyncio.to_thread(self.adapter.speech_available))[0]
        return await asyncio.to_thread(native_status, self.adapter.home, mode)

    async def handle(self, peer, value):
        validate_request(value)
        base = {'rid': value['rid'], 'target': value['target'], 'cursor': value.get('cursor', 0)}
        if peer.identity in self.operations:
            await peer.send('agent_options', **base, total=0, error='busy')
            return
        self.operations.add(peer.identity)
        try:
            async with asyncio.timeout(8):
                await self.request(peer, value, base)
        except (ControlError, TimeoutError, OSError, ValueError) as error:
            if self.current(peer):
                code = error.code if isinstance(error, ControlError) else 'unavailable'
                await peer.send('agent_options', **base, total=0, error=code)
        finally:
            self.operations.discard(peer.identity)

    async def request(self, peer, value, base):
        if not self.current(peer):
            raise ControlError('denied')
        model, rows, caps = await self.catalogue(peer, value['target'])
        if value['t'] == 'agent_model':
            self.check_selection(peer, value, rows)
            field = 'mode' if value['target'] == 'mode' else self.mode(peer)
            self.store.select(peer.identity, field, value['id'])
            model, rows, caps = await self.catalogue(peer, value['target'])
        if not self.current(peer):
            return
        cursor = min(base['cursor'], max(0, (len(rows) - 1) // 4))
        await peer.send('agent_options', **{**base, 'cursor': cursor}, model=model,
                        models=rows[cursor * 4:cursor * 4 + 4], total=len(rows),
                        stt=caps['stt'], tts=caps['tts'])
        if value['t'] == 'agent_model':
            await self.publish(peer)

    def check_selection(self, peer, value, rows):
        if not self.current(peer):
            raise ControlError('denied')
        if (peer.native or peer.turn is not None or (peer.task and not peer.task.done()) or
                self.adapter.native_jobs.busy(peer.identity)):
            raise ControlError('busy')
        row = next((item for item in rows if item['id'] == value['id']), None)
        if row is None:
            raise ControlError('invalid_model')
        if not row['available']:
            raise ControlError('unavailable')


class ControlError(ValueError):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


def validate_request(value):
    if value.get('target') not in ('agent', 'mode', 'voice', 'stt', 'tts'):
        raise ValueError('Invalid catalogue target')
    for name, limit in [('rid', 65535), ('cursor', 255)]:
        field = value.get(name, 0 if name == 'cursor' else None)
        if type(field) is not int or not 0 <= field <= limit:
            raise ValueError('Invalid catalogue request')
    if value['t'] == 'agent_model':
        model = value.get('id')
        if not isinstance(model, str) or not model or len(model) > 160 or any(ord(c) < 32 for c in model):
            raise ValueError('Invalid catalogue selection')
