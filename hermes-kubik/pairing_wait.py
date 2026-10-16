"""Socket-owned pairing notifications: optional Telegram never blocks local approval."""
import asyncio
import contextlib
import json
import time

from aiohttp import WSMsgType


async def announce_pairing(adapter, socket, identity, code):
    delay = 15
    while True:
        notifier = getattr(adapter, 'telegram_pairing', None)
        if notifier is None:
            return
        try:
            delivered = await notifier.notify(identity, code)
        except Exception:
            delivered = False  # Telegram is optional, including during startup or shutdown.
        if delivered:
            if not socket.closed and not adapter.security.approved(identity):
                await socket.send_json({'t': 'pair', 'code': code, 'approval': 'telegram'})
            return
        await asyncio.sleep(delay)
        delay = 60


async def wait_pairing(adapter, socket, identity, code, telegram=False):
    if adapter.security.approved(identity):
        return
    pending = adapter.security.read_pending().get(identity)
    if not pending or pending['code'] != code:
        raise TimeoutError('Pairing expired')
    remaining = min(600, max(0, pending['expires'] - time.time()))
    notice = None if telegram else asyncio.create_task(announce_pairing(adapter, socket, identity, code))
    try:
        async with asyncio.timeout(remaining):
            while not adapter.security.approved(identity):
                await pairing_ping(socket)
    finally:
        if notice is not None:
            notice.cancel()
            # Own the task through disconnect, approval and timeout; retrieve late failures.
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await notice


async def pairing_ping(socket):
    try:
        message = await socket.receive(timeout=2)
    except asyncio.TimeoutError:
        return
    if message.type != WSMsgType.TEXT:
        raise ValueError('Pairing closed')
    value = json.loads(message.data)
    if not isinstance(value, dict) or value.get('t') != 'ping':
        raise ValueError('Pairing only accepts ping')
    await socket.send_json({'t': 'pong', 'ts': value.get('ts')})
