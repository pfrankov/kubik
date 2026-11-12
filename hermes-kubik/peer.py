"""Connection-owned device state; no state leaks to a replacement socket."""
import asyncio
import secrets

from .printer_wire import dumps


def version(hello, name):
    value = hello.get(name)
    if value is not None and (type(value) is not int or not 1 <= value <= 255):
        raise ValueError('Invalid ' + name + ' version')
    return value


class Peer:
    def __init__(self, socket, identity, hello):
        self.socket, self.identity = socket, identity
        self.ready = False
        self.transport = None
        self.native = None
        self.session_id = secrets.token_hex(8)
        self.screen_emoji_version = version(hello, 'screen_emoji')
        self.printer_version = version(hello, 'printer')
        self.printer_image_version = version(hello, 'printer_image')
        self.printer_preview_version = version(hello, 'printer_preview')
        self.screen_image_version = version(hello, 'screen_image')
        self.volume = hello.get('volume', 0)
        if type(self.volume) is not int or not 0 <= self.volume <= 100:
            raise ValueError('Invalid volume')
        self.turn = None
        self.pcm = bytearray()
        self.recorded_at = 0
        self.task = None
        self.gen = 0
        self.played_ms = 0
        self.progress = asyncio.Event()
        self.receipts = {}
        self.image_receipts = {}
        self.image_busy = False
        self.complete = asyncio.Event()
        self.message_id = None
        self.session_key = None
        self.input_turn = None
        self.stage = "idle"
        self.muted = False
        self.output_lock = asyncio.Lock()
        self.output_task = None
        self.sent_ms = 0
        self.waiting = False
        self.pending_previous = None
    async def send(self, message_type, **fields):
        options = {'dumps': dumps} if message_type == 'printer' else {}
        await self.socket.send_json({'t': message_type, **fields}, **options)


