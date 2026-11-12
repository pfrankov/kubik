"""Sequential page transmission under one output lease and one bounded tool call."""
import asyncio
import hashlib

from . import printer_image_stream as stream


def page_id(rid, index):
    return rid if index == 0 else hashlib.sha256(f'{rid}:page:{index}'.encode()).hexdigest()[:32]


def summary(row, results):
    layouts = [item['layout'] for item in results]
    first = results[0]
    return {'rid': row['rid'], 'status': 'sent', 'outcome': 'submitted', 'printer': first['printer'],
            'layout': {'format': 'document', 'width_dots': max(x['width_dots'] for x in layouts),
                       'height_dots': sum(x['height_dots'] for x in layouts),
                       'width_mm': max(x['width_mm'] for x in layouts),
                       'height_mm': sum(x['height_mm'] for x in layouts)},
            'pagesSubmitted': len(results), 'pagesTotal': len(row['pages']),
            'pageLayouts': layouts, 'paperConfirmed': False}


def failed(row, value, submitted):
    if not submitted:
        return value
    return {'rid': row['rid'], 'status': 'error', 'code': value.get('code', 'document_incomplete'),
            'outcome': 'unknown' if value.get('outcome') == 'unknown' else 'partial',
            'pagesSubmitted': submitted, 'pagesTotal': len(row['pages']), 'paperConfirmed': False}


async def transmit(jobs, row):
    results = []
    for index, raster in enumerate(row['pages']):
        if row['future'].done():
            return
        row['wire_rid'] = page_id(row['rid'], index)
        row['raster'] = raster
        if index:
            row['quiescent'] = asyncio.get_running_loop().create_future()
            row['credits'] = asyncio.Queue(maxsize=1)
        async with asyncio.timeout(jobs.hard_guard):
            await stream.transmit(row)
            value = await asyncio.shield(row['quiescent'])
        if value is None:
            return
        if value.get('status') != 'sent':
            jobs.finish(row, failed(row, value, len(results)))
            return
        results.append(value)
        row['pages_submitted'] = len(results)
    jobs.finish(row, summary(row, results))
