"""One-credit image transmission; never resend a possibly printed band."""
import asyncio


async def transmit(row):
    peer, raster = row['peer'], row['raster']
    await peer.send('printer_image', rid=row.get('wire_rid', row['rid']), action='start', head_width=raster.head_width,
                    width=raster.width, height=raster.height)
    offset = 0
    while offset < raster.height and not row.get('aborting'):
        credit_task = asyncio.create_task(row['credits'].get())
        try:
            done, _ = await asyncio.wait((credit_task, row['quiescent']), return_when=asyncio.FIRST_COMPLETED)
            if row['quiescent'] in done:
                return
            credit = credit_task.result()
        finally:
            if not credit_task.done():
                credit_task.cancel()
            await asyncio.gather(credit_task, return_exceptions=True)
        if row.get('aborting'):
            return
        if credit != offset:
            raise ValueError('invalid_image_credit')
        count = min(16, raster.height - offset)
        frame = b'\x06\x00' + row.get('wire_rid', row['rid']).encode('ascii') + offset.to_bytes(2, 'big') + bytes([count])
        await peer.socket.send_bytes(frame + raster.block(offset, count))
        offset += count


async def abort(row):
    try:
        async with asyncio.timeout(3):
            await row['peer'].send('printer_image', rid=row.get('wire_rid', row['rid']), action='abort')
    except Exception:
        pass


def accept_credit(row, peer, value):
    if (not row or row['peer'] is not peer or not row.get('raster') or not row['dispatched']
            or set(value) != {'t', 'rid', 'offset'} or value.get('rid') != row.get('wire_rid', row['rid'])
            or type(value.get('offset')) is not int or not 0 <= value['offset'] < row['raster'].height):
        return False
    try:
        row['credits'].put_nowait(value['offset'])
    except asyncio.QueueFull:
        return False
    return True
