"""Connection-owned device state; no state leaks to a replacement socket."""
import asyncio
import secrets


class Peer:
    def __init__(self, socket, identity, hello):
        self.socket, self.identity = socket, identity
        self.ready = False
        self.native = None
        self.session_id = secrets.token_hex(8)
        self.printer_version = hello.get('printer')
        if self.printer_version is not None and (type(self.printer_version) is not int or not 1 <= self.printer_version <= 255):
            raise ValueError('Invalid printer version')
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
        await self.socket.send_json({'t': message_type, **fields})


