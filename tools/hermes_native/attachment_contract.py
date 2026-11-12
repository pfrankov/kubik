"""Exercise the real Hermes MEDIA response path on a simulated Kubik; no paid calls."""
from pathlib import Path


class ImageReceiver:
    def __init__(self, device):
        self.device = device
        self.binary, self.acknowledge = device.binary, device.acknowledge
        self.data, self.rid, self.shows = bytearray(), None, 0
        self.printer_commands = 0
        self.status = 'shown'

    async def receive_binary(self, data):
        if data[0] == 6:
            self.printer_commands += 1
            raise AssertionError('Attachment delivery must not print')
        if data[0] != 7:
            return await self.binary(data)
        assert data[1] == 1 and 37 <= len(data) <= 932
        rid, offset = data[2:34].decode('ascii'), int.from_bytes(data[34:36], 'big')
        assert (self.rid is None or self.rid == rid) and offset == len(self.data)
        self.rid = rid
        self.data.extend(data[36:])
        assert len(self.data) <= 8224

    async def receive_json(self, value):
        if value.get('t') in ('printer', 'printer_image'):
            self.printer_commands += 1
            raise AssertionError('Attachment delivery must not print')
        if value.get('t') == 'screen_image' and value.get('action') == 'show':
            assert self.rid == value['rid'] and len(self.data) == 8224
            self.shows += 1
            response = {'t': 'screen_image_result', 'rid': self.rid, 'status': self.status}
            if self.status == 'error':
                response['code'] = 'unavailable'
            await self.device.send(**response)
            return
        await self.acknowledge(value)

    def install(self):
        self.device.binary, self.device.acknowledge = self.receive_binary, self.receive_json

    def restore(self):
        self.device.binary, self.device.acknowledge = self.binary, self.acknowledge

    def next_attempt(self):
        self.data, self.rid, self.shows = bytearray(), None, 0


async def deliver_response(adapter, event, path):
    response = 'MEDIA:' + str(path)
    key = adapter._event_session_key(event)
    extracted = await adapter._extract_response_content(response, event, key, is_ephemeral_response=False)
    assert (len(extracted.media_files) == 1 and Path(extracted.media_files[0][0]).resolve() == path.resolve()
            and extracted.media_files[0][1] is False and not extracted.images and not extracted.local_files), extracted
    results = []
    await adapter._deliver_attachments(event, extracted, {}, anything_sent=False, record_delivery=results.append)
    assert len(results) == 1
    return results[0]


async def check_attachment_delivery(adapter, device, peer, event, path):
    assert peer.screen_image_version == 1
    receiver = ImageReceiver(device)
    initial_events = len(device.events)
    receiver.install()
    try:
        result = await deliver_response(adapter, event, path)
        assert result.success and receiver.shows == 1 and receiver.printer_commands == 0
        messages = [item.get('text', '') for item in device.events[initial_events:] if item.get('t') == 'text']
        assert not any("Couldn't deliver" in text for text in messages), messages
        receiver.next_attempt()
        receiver.status = 'error'
        result = await deliver_response(adapter, event, path)
        assert not result.success and receiver.shows == 1 and receiver.printer_commands == 0
    finally:
        receiver.restore()
    print('PASS Hermes response MEDIA extraction, native delivery and shown/error acknowledgements without printing', flush=True)
