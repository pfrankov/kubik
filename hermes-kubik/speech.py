"""Bounded PCM conversion and playback paced by the device's actual consumed audio."""
import asyncio
from pathlib import Path

from gateway.platforms.base import SendResult
from .codec import encode

RATE = 24000
MAX_BYTES = 120 * RATE * 2


async def pcm_file(path):
    if not Path(path).is_file() or Path(path).stat().st_size > 32 * 1024 * 1024:
        raise ValueError('Invalid speech file')
    process = await asyncio.create_subprocess_exec('ffmpeg', '-v', 'error', '-i', str(path),
        '-f', 's16le', '-ac', '1', '-ar', str(RATE), 'pipe:1',
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
    try:
        async with asyncio.timeout(30):
            result = bytearray()
            while block := await process.stdout.read(8192):
                result.extend(block)
                if len(result) > MAX_BYTES:
                    raise ValueError('Speech exceeds 120 seconds')
            if await process.wait() or not result or len(result) % 4:
                raise ValueError('Invalid speech PCM')
            return result
    finally:
        if process.returncode is None:
            process.kill()
        await process.wait()


async def send_pcm(peer, pcm):
    state, sent = [0, 0], 0
    peer.gen = (peer.gen + 1) % 256
    peer.played_ms = 0
    peer.sent_ms = max(1, len(pcm) // 48)
    done = asyncio.Event()
    peer.receipts[('played', peer.gen)] = done
    try:
        await peer.send('speak', gen=peer.gen, kind='reply')
        for start in range(0, len(pcm), 1920):
            # At most 600 ms ahead: includes in-flight packets and stays below the 900 ms ring.
            while sent - peer.played_ms > 560:
                peer.progress.clear()
                await asyncio.wait_for(peer.progress.wait(), 10)
                if peer.socket.closed:
                    raise ConnectionError('Disconnected')
            block = pcm[start:start + 1920]
            await peer.socket.send_bytes(bytes((3, peer.gen)) + encode(block, state))
            sent += len(block) / 48
        await peer.send('speak_end', gen=peer.gen)
        await asyncio.wait_for(done.wait(), 12)
        if peer.socket.closed:
            raise ConnectionError('Disconnected')
        return SendResult(success=True, message_id=str(peer.gen))
    finally:
        peer.receipts.pop(('played', peer.gen), None)


async def play_file(peer, path):
    try:
        pcm = await pcm_file(path)
        async with asyncio.timeout(140):
            return await send_pcm(peer, pcm)
    except asyncio.CancelledError:
        await peer.send('speak_cancel', gen=peer.gen)
        raise
    except (ValueError, OSError, TimeoutError, ConnectionError):
        await peer.send('speak_cancel', gen=peer.gen)
        return SendResult(success=False, error='Speech delivery failed', retryable=True)
