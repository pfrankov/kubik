"""Connection-bound printer requests; uncertain side effects are never replayed."""
import asyncio
import hashlib
import json
import time

from . import printer_contract as contract
from .printer_emoji import EmojiAtlas, EmojiError
from .printer_wire import packet
from . import printer_image_stream as image_stream
from . import printer_batch
from .native_jobs import extend_deadline


class PrinterFailure(Exception):
    def __init__(self, code, *, outcome='not_sent', rid=None):
        super().__init__(code)
        self.code, self.outcome, self.rid = code, outcome, rid

    def value(self):
        return {'status': 'error', 'code': self.code, 'outcome': self.outcome,
                **({'rid': self.rid} if self.rid else {}), 'paperConfirmed': False}


class PrinterJobs:
    def __init__(self, adapter, *, now=time.monotonic, status_timeout=20, print_timeout=120, hard_guard=95):
        self.adapter, self.now = adapter, now
        self.timeouts = {'status': status_timeout, 'print': print_timeout}
        self.records, self.pending, self.tasks = {}, {}, set()
        self.hard_guard = hard_guard
        self.atlas = EmojiAtlas(adapter.home / 'kubik/apple-emoji.bin')

    def current(self, peer):
        return self.adapter._ready_peer(peer.identity) is peer and bool(self.adapter.config.enabled)

    def prune(self):
        cutoff = self.now() - 600
        expired = [rid for rid, row in self.records.items() if row['finished'] is not None and not row['uncertain'] and row['finished'] < cutoff]
        for rid in expired:
            del self.records[rid]

    async def request(self, peer, call_id, arguments, event_id, raster=None):
        try:
            values = self.validate_request(peer, call_id, arguments, event_id, raster)
        except PrinterFailure as error:
            return error.value()
        self.prune()
        rid = hashlib.sha256(f'{peer.identity}\n{event_id}\n{call_id}'.encode()).hexdigest()[:32]
        payload = json.dumps(values, sort_keys=True, separators=(',', ':'), ensure_ascii=False)
        try:
            self.admit(peer, rid, payload)
        except PrinterFailure as error:
            return error.value()
        previous = self.records.get(rid)
        if previous:
            return await asyncio.shield(previous['future'])
        image_error = self.image_admission(peer, values, rid)
        if image_error:
            return image_error
        try:
            pages = self.request_pages(values, raster, rid)
        except PrinterFailure as error:
            return error.value()
        loop = asyncio.get_running_loop()
        duration = self.request_duration(values)
        try:
            self.extend_native_document(peer, event_id, values, duration)
        except PrinterFailure as error:
            return error.value()
        row = {'peer': peer, 'rid': rid, 'payload': payload, 'parameters': values, 'action': values['action'], 'dispatched': False,
               'future': loop.create_future(), 'quiescent': loop.create_future(), 'finished': None, 'uncertain': False,
               'deadline': loop.time() + duration, 'raster': pages[0] if pages else None, 'pages': pages,
               'credits': asyncio.Queue(maxsize=1), 'pages_submitted': 0}
        self.records[rid] = self.pending[peer.identity] = row
        row['task'] = asyncio.create_task(self.operate(row))
        self.tasks.add(row['task'])
        row['task'].add_done_callback(self.tasks.discard)
        try:
            async with asyncio.timeout_at(row['deadline']):
                return await asyncio.shield(row['future'])
        except TimeoutError:
            await self.abandon(row, 'timeout')
            return row['future'].result()
        except asyncio.CancelledError:
            await self.abandon(row, 'cancelled')
            raise

    def validate_request(self, peer, call_id, arguments, event_id, raster):
        values = contract.parameters(arguments)
        if values is None or not contract.safe_string(call_id, 256) or not contract.safe_string(event_id, 256):
            raise PrinterFailure('invalid_input')
        if values.get('format') in ('image', 'document') and (raster is None or getattr(peer, 'printer_image_version', None) != 1):
            raise PrinterFailure('printer_unsupported')
        return values

    def request_pages(self, values, raster, rid):
        pages = tuple(raster) if isinstance(raster, list) else ((raster,) if raster is not None else ())
        if values.get('format') == 'document' and not 1 <= len(pages) <= 4:
            raise PrinterFailure('invalid_input', rid=rid)
        return pages

    def request_duration(self, values):
        return 4 * self.hard_guard + 30 if values.get('format') == 'document' else self.timeouts[values['action']]

    def extend_native_document(self, peer, event_id, values, duration):
        if values.get('format') != 'document':
            return
        job = self.adapter.native_jobs.jobs.get(event_id)
        if job and job.peer is peer:
            allowed = job.extend_for_document(duration + 10)
        elif event_id in getattr(peer, 'classic_deadlines', {}):
            allowed = extend_deadline(*peer.classic_deadlines[event_id], duration + 10)
        else:
            return
        if not allowed:
            raise PrinterFailure('deadline_exceeded')

    def image_admission(self, peer, values, rid):
        images = getattr(peer, 'printer_images', None)
        if values.get('format') == 'document' and self.document_uncertain(peer, values):
            return PrinterFailure('outcome_unknown', outcome='unknown', rid=rid).value()
        if images is None:
            return None
        for ref in contract.image_refs(values):
            if images.is_uncertain(ref):
                return PrinterFailure('outcome_unknown', outcome='unknown', rid=rid).value()
            if images.get(ref) is None:
                return PrinterFailure('invalid_image_ref', rid=rid).value()
        return None

    def document_uncertain(self, peer, values):
        key = document_key(values)
        return any(row['uncertain'] and row['peer'].identity == peer.identity and
                   row['parameters'].get('format') == 'document' and document_key(row['parameters']) == key
                   for row in self.records.values())

    def admit(self, peer, rid, payload):
        if not self.current(peer):
            raise PrinterFailure('device_replaced', rid=rid)
        previous = self.records.get(rid)
        if previous:
            if previous['payload'] != payload:
                raise PrinterFailure('request_conflict', rid=rid)
            return
        if peer.printer_version != contract.VERSION:
            raise PrinterFailure('printer_unsupported', rid=rid)
        if len(self.records) >= 64 or peer.identity in self.pending:
            raise PrinterFailure('printer_busy', rid=rid)

    async def operate(self, row):
        locked = False
        try:
            async with asyncio.timeout_at(row['deadline']):
                await row['peer'].output_lock.acquire()
                locked = True
                await self.preflight(row)
            if not row['future'].done():
                await self.device_work(row)
        except asyncio.CancelledError:
            self.finish(row, self.failure(row, 'cancelled'))
            if row['dispatched']:
                await self.break_connection(row['peer'])
        except PrinterFailure as error:
            self.finish(row, error.value())
        except TimeoutError:
            self.finish(row, self.failure(row, 'timeout'))
        except Exception:
            self.finish(row, self.failure(row, 'disconnected'))
            if row['dispatched']:
                await self.break_connection(row['peer'])
        finally:
            row['pages'] = ()
            row['raster'] = None
            if locked:
                row['peer'].output_lock.release()
            if self.pending.get(row['peer'].identity) is row:
                self.pending.pop(row['peer'].identity)

    async def preflight(self, row):
        if row['future'].done():
            return
        if not self.current(row['peer']):
            raise PrinterFailure('unauthorized', rid=row['rid'])
        await self.output_fence(row['peer'])
        if not self.current(row['peer']):
            raise PrinterFailure('unauthorized', rid=row['rid'])
        await self.prepare_wire(row)
        if not self.current(row['peer']):
            raise PrinterFailure('unauthorized', rid=row['rid'])

    async def prepare_wire(self, row):
        values = row['parameters']
        if row['raster'] is not None:
            return
        try:
            glyphs = await asyncio.to_thread(self.atlas.glyphs, values['text']) if values.get('format') == 'text' else []
            row['wire'] = packet(row['rid'], values, glyphs)
        except EmojiError as error:
            raise PrinterFailure(error.code, rid=row['rid']) from None
        except ValueError:
            raise PrinterFailure('invalid_request', rid=row['rid']) from None

    async def output_fence(self, peer):
        if not peer.gen:
            return
        key, receipt = ('cancelled', peer.gen), asyncio.Event()
        peer.receipts[key] = receipt
        try:
            await peer.send('speak_cancel', gen=peer.gen)
            async with asyncio.timeout(3):
                await receipt.wait()
        finally:
            peer.receipts.pop(key, None)

    async def device_work(self, row):
        if row['future'].done():
            return
        if asyncio.get_running_loop().time() >= row['deadline']:
            self.finish(row, self.failure(row, 'timeout'))
            return
        if not self.current(row['peer']):
            self.finish(row, self.failure(row, 'unauthorized'))
            return
        row['dispatched'] = True
        try:
            if len(row['pages']) > 1:
                await printer_batch.transmit(self, row)
                return
            async with asyncio.timeout(self.hard_guard):
                try:
                    if row['raster'] is not None:
                        await image_stream.transmit(row)
                    else:
                        await row['peer'].send('printer', **{key: value for key, value in row['wire'].items() if key != 't'})
                except Exception:
                    self.finish(row, self.failure(row, 'disconnected'))
                    if row['peer'].socket.closed:
                        self.close(row['peer'])
                value = await asyncio.shield(row['quiescent'])
                if value is not None:
                    self.finish(row, value)
        except TimeoutError:
            self.finish(row, self.failure(row, 'timeout'))
            await self.break_connection(row['peer'])

    async def break_connection(self, peer):
        peer.ready = False
        try:
            async with asyncio.timeout(3):
                await peer.socket.close(code=4001)
        except Exception:
            pass
        finally:
            self.close(peer)

    async def abandon(self, row, code):
        if row['dispatched'] and row['raster'] is not None:
            row['aborting'] = True
        self.finish(row, self.failure(row, code))
        if row['dispatched'] and row['raster'] is not None:
            await image_stream.abort(row)
        if not row['dispatched']:
            row['task'].cancel()
            await asyncio.gather(row['task'], return_exceptions=True)

    def failure(self, row, code):
        uncertain = row['dispatched'] and row['action'] == 'print'
        value = PrinterFailure(code, rid=row['rid'], outcome='unknown' if uncertain else 'not_sent').value()
        if row['parameters'].get('format') == 'document':
            value.update(pagesSubmitted=row['pages_submitted'], pagesTotal=len(row['pages']))
        return value

    def finish(self, row, value):
        if row['future'].done():
            return
        row['finished'] = self.now()
        row['uncertain'] = value.get('outcome') in ('unknown', 'partial')
        images = getattr(row['peer'], 'printer_images', None)
        if row['uncertain'] and row['raster'] is not None:
            if images is not None:
                for ref in contract.image_refs(row['parameters']):
                    images.mark_uncertain(ref)
        row['future'].set_result(value)

    def accept(self, peer, value):
        row = self.pending.get(peer.identity)
        if not row or row['peer'] is not peer or not row['dispatched'] or value.get('rid') != row.get('wire_rid', row['rid']):
            return False
        if not self.current(peer):
            self.finish(row, self.failure(row, 'device_replaced'))
        elif not self.valid_response(row, value):
            self.finish(row, self.failure(row, 'invalid_response'))
        else:
            result = {key: item for key, item in value.items() if key != 't'} | {'paperConfirmed': False}
            if row['parameters'].get('format') == 'document':
                if result.get('layout'):
                    result['layout'] = {**result['layout'], 'format': 'document'}
                if len(row['pages']) == 1:
                    result.update(pagesSubmitted=int(result['status'] == 'sent'), pagesTotal=1)
            if not row['quiescent'].done():
                row['quiescent'].set_result(result)
        return True

    def valid_response(self, row, value):
        raster = row['raster']
        if not contract.result(value, row['action'], image=raster is not None):
            return False
        if not contract.requested_layout(value, row['parameters']):
            return False
        layout = value.get('layout')
        return not (raster and layout) or (layout['width_dots'], layout['height_dots']) == (raster.width, raster.height)

    def accept_credit(self, peer, value):
        row = self.pending.get(peer.identity)
        if not self.current(peer) or not image_stream.accept_credit(row, peer, value):
            if row and row['peer'] is peer and row.get('raster') and row['dispatched']:
                row['task'].cancel()
            return False
        return True

    def close(self, peer):
        row = self.pending.get(peer.identity)
        if row and row['peer'] is peer:
            self.finish(row, self.failure(row, 'disconnected'))
            if not row['quiescent'].done():
                row['quiescent'].set_result(None)
            if (not row['dispatched'] or peer.socket.closed) and row['task'] is not asyncio.current_task():
                row['task'].cancel()

    async def shutdown(self):
        for row in tuple(self.pending.values()):
            await self.break_connection(row['peer'])
        await asyncio.gather(*tuple(self.tasks), return_exceptions=True)


def document_key(values):
    blocks = [{key: block[key] for key in ('type', 'text', 'image_ref') if key in block}
              for block in values['blocks']]
    return hashlib.sha256(json.dumps(blocks, sort_keys=True, ensure_ascii=False).encode()).hexdigest()
