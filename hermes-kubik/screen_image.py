"""Deliver generated image attachments only to their owning connected Kubik."""
import asyncio
import contextlib
import secrets

from gateway.platforms.base import SendResult
from .printer_image_origin import generated_image
from .printer_preview import make_preview
from .printer_image_raster import RasterError

TIMEOUT = 5


def owned_image(home, peer, path):
    if not isinstance(path, str) or not 1 <= len(path) <= 4096:
        return None
    images = getattr(peer, 'printer_images', None)
    if images is None:
        return None
    try:
        data = generated_image(home, {'success': True, 'provider': 'openai', 'image': path})
        return data if data is not None and images.contains(data) else None
    except (TypeError, ValueError, OSError, KeyError):
        return None


def valid_result(value):
    if not isinstance(value, dict) or value.get('t') != 'screen_image_result':
        return False
    rid, status = value.get('rid'), value.get('status')
    if not isinstance(rid, str) or len(rid) != 32 or any(c not in '0123456789abcdef' for c in rid):
        return False
    if status == 'shown':
        return set(value) == {'t', 'rid', 'status'}
    if status == 'error':
        code = value.get('code')
        return set(value) == {'t', 'rid', 'status', 'code'} and isinstance(code, str) and 1 <= len(code) <= 64
    return False


def accept(peer, value):
    if not valid_result(value):
        return False
    future = peer.image_receipts.get(value['rid'])
    if future is None or future.done():
        return False
    future.set_result(value)
    return True


def close(peer):
    for future in tuple(peer.image_receipts.values()):
        if not future.done():
            future.set_result({'status': 'error', 'code': 'device_disconnected'})


def current(adapter, peer, data):
    return adapter._ready_peer(peer.identity) is peer and not peer.muted and peer.printer_images.contains(data)


async def write_image(adapter, peer, data, rid):
    if not current(adapter, peer, data):
        raise ConnectionError('Kubik disconnected')
    encoded = await asyncio.to_thread(make_preview, data)
    if not current(adapter, peer, data):
        raise ConnectionError('Kubik disconnected')
    for offset in range(0, len(encoded), 896):
        await peer.socket.send_bytes(b'\x07\x01' + rid.encode('ascii') + offset.to_bytes(2, 'big') + encoded[offset:offset + 896])
    await peer.send('screen_image', action='show', rid=rid)


async def cancel_image(adapter, peer, rid):
    if adapter._ready_peer(peer.identity) is peer:
        with contextlib.suppress(Exception):
            async with asyncio.timeout(1):
                await peer.send('screen_image', action='cancel', rid=rid)


async def transfer(adapter, peer, data):
    rid = secrets.token_hex(16)
    future = asyncio.get_running_loop().create_future()
    peer.image_receipts[rid] = future
    locked = dispatched = shown = False
    try:
        async with asyncio.timeout(TIMEOUT):
            await peer.output_lock.acquire()
            locked = dispatched = True
            await write_image(adapter, peer, data, rid)
        async with asyncio.timeout(TIMEOUT):
            result = await future
        if adapter._ready_peer(peer.identity) is not peer or result.get('status') != 'shown':
            return SendResult(success=False, error='Kubik could not show image')
        shown = True
        return SendResult(success=True, message_id=rid)
    except (TimeoutError, ConnectionError, OSError, RuntimeError, RasterError, MemoryError):
        return SendResult(success=False, error='Kubik could not show image')
    finally:
        if dispatched and not shown:
            await cancel_image(adapter, peer, rid)
        peer.image_receipts.pop(rid, None)
        if not future.done():
            future.cancel()
        if locked:
            peer.output_lock.release()


async def send_file(adapter, chat_id, image_path, caption=None, reply_to=None, metadata=None):
    peer = adapter._ready_peer(chat_id)
    if not peer:
        return SendResult(success=False, error='Kubik disconnected', retryable=True)
    if peer.screen_image_version != 1 or peer.muted:
        return SendResult(success=False, error='Kubik cannot show image')
    if peer.image_busy:
        return SendResult(success=False, error='Kubik image display busy', retryable=True)
    peer.image_busy = True
    try:
        return await send_admitted(adapter, peer, chat_id, image_path, caption, reply_to, metadata)
    finally:
        peer.image_busy = False


async def send_admitted(adapter, peer, chat_id, image_path, caption, reply_to, metadata):
    try:
        async with asyncio.timeout(TIMEOUT):
            data = await asyncio.to_thread(owned_image, adapter.home, peer, image_path)
        if data is None:
            return SendResult(success=False, error='Image is unavailable to this Kubik')
        if adapter._ready_peer(chat_id) is not peer:
            return SendResult(success=False, error='Kubik disconnected', retryable=True)
        if caption:
            shown = await adapter.send(chat_id, caption, reply_to=reply_to, metadata=metadata)
            if not shown.success:
                return shown
        return await transfer(adapter, peer, data)
    except TimeoutError:
        return SendResult(success=False, error='Image is unavailable to this Kubik')


class AttachmentMixin:
    async def send_image_file(self, chat_id, image_path, caption=None, reply_to=None, metadata=None, **kwargs):
        return await send_file(self, chat_id, image_path, caption, reply_to, metadata)
